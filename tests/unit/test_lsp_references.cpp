#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/global_index.hpp"
#include "roxy/lsp/lsp_analysis_context.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/core/tsl/robin_set.h"

#include <cstring>

using namespace rx;

// Symbol category for reference searching (mirrors server.cpp)
enum class SymbolCategory : u8 {
    Function,
    Struct,
    Enum,
    Trait,
    Global,
    Method,
    Field,
    Constructor,
    Local,
    Parameter,
};

struct SymbolIdentity {
    SymbolCategory category;
    String name;
    String qualifier;
    TextRange enclosing_range;
    String enclosing_uri;
};

// Walk up parent chain to find enclosing function
static SyntaxNode* find_enclosing_function(SyntaxNode* node) {
    SyntaxNode* current = node ? node->parent : nullptr;
    while (current) {
        if (current->kind == SyntaxKind::NodeFunDecl ||
            current->kind == SyntaxKind::NodeMethodDecl ||
            current->kind == SyntaxKind::NodeConstructorDecl ||
            current->kind == SyntaxKind::NodeDestructorDecl) {
            return current;
        }
        current = current->parent;
    }
    return nullptr;
}

// Collect identifiers matching name
static void collect_identifiers(SyntaxNode* root, StringView name, Vector<SyntaxNode*>& out) {
    if (!root) return;
    if (root->kind == SyntaxKind::TokenIdentifier && root->token.text() == name) {
        out.push_back(root);
    } else if (root->kind == SyntaxKind::TokenKwSelf && name == StringView("self")) {
        out.push_back(root);
    }
    for (u32 i = 0; i < root->children.size(); i++) {
        collect_identifiers(root->children[i], name, out);
    }
}

// Helper: check if identifier is a local variable in the enclosing function
static bool is_local_variable(SyntaxNode* candidate, StringView candidate_text) {
    SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
    if (!enclosing_fn) return false;
    BumpAllocator ast_allocator(4096);
    CstLowering lowering(ast_allocator);
    Decl* ast_decl = lowering.lower_decl(enclosing_fn);
    tsl::robin_set<String> local_names;
    LspAnalysisContext::collect_local_var_names(ast_decl, local_names);
    return local_names.count(String(candidate_text)) > 0;
}

// Helper: resolve receiver type for a dot-access expression
static String resolve_receiver_type(SyntaxNode* object_expr, SyntaxNode* enclosing_fn,
                                     LspAnalysisContext* analysis_ctx) {
    if (!analysis_ctx || !enclosing_fn) return String();
    BumpAllocator ast_allocator(8192);
    BodyAnalysisResult body_result = analysis_ctx->analyze_function_body(
        enclosing_fn, ast_allocator);
    if (!body_result.decl) return String();
    tsl::robin_map<String, Type*> local_vars;
    analysis_ctx->collect_local_variables(body_result.decl, local_vars);
    Type* recv_type = analysis_ctx->resolve_cst_expr_type(object_expr, local_vars);
    if (recv_type && !recv_type->is_error()) {
        return LspAnalysisContext::type_to_string(recv_type);
    }
    return String();
}

// Forward declarations of functions mirroring server.cpp logic
static bool identify_symbol_at_cursor(SyntaxNode* node, const GlobalIndex& index,
                                       StringView uri, SymbolIdentity& out_identity,
                                       LspAnalysisContext* analysis_ctx = nullptr);
static bool is_reference_to_symbol(SyntaxNode* candidate, const SymbolIdentity& target,
                                    const GlobalIndex& index, SyntaxNode* file_root,
                                    StringView file_uri,
                                    LspAnalysisContext* analysis_ctx = nullptr);

// --- identify_symbol_at_cursor ---
static bool identify_symbol_at_cursor(SyntaxNode* node, const GlobalIndex& index,
                                       StringView uri, SymbolIdentity& out_identity,
                                       LspAnalysisContext* analysis_ctx) {
    if (!node) return false;

    if (node->kind == SyntaxKind::TokenKwSelf) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (!enclosing_fn) return false;
        for (u32 i = 0; i < enclosing_fn->children.size(); i++) {
            if (enclosing_fn->children[i]->kind == SyntaxKind::TokenIdentifier) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(enclosing_fn->children[i]->token.text());
                return true;
            }
        }
        return false;
    }

    if (node->kind != SyntaxKind::TokenIdentifier) return false;

    StringView identifier = node->token.text();
    SyntaxNode* parent = node->parent;

    if (parent) {
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == node;
            if (is_member_name) {
                SyntaxNode* object_expr = parent->children[0];
                SyntaxNode* enclosing_fn = find_enclosing_function(parent);
                String receiver_type = resolve_receiver_type(object_expr, enclosing_fn, analysis_ctx);
                if (!receiver_type.empty()) {
                    StringView current_type(receiver_type.data(), receiver_type.size());
                    u32 depth = 0;
                    while (!current_type.empty() && depth < 16) {
                        if (index.find_field(current_type, identifier)) {
                            out_identity.category = SymbolCategory::Field;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(current_type);
                            return true;
                        }
                        if (index.find_method(current_type, identifier)) {
                            out_identity.category = SymbolCategory::Method;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(current_type);
                            return true;
                        }
                        current_type = index.find_struct_parent(current_type);
                        depth++;
                    }
                }
                return false;
            }
        }

        if (parent->kind == SyntaxKind::NodeTypeExpr) {
            if (index.find_struct(identifier)) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(identifier);
                return true;
            }
            if (index.find_enum(identifier)) {
                out_identity.category = SymbolCategory::Enum;
                out_identity.name = String(identifier);
                return true;
            }
            if (index.find_trait(identifier)) {
                out_identity.category = SymbolCategory::Trait;
                out_identity.name = String(identifier);
                return true;
            }
            return false;
        }

        if (parent->kind == SyntaxKind::NodeCallExpr) {
            bool is_callee = parent->children.size() > 0 && parent->children[0] == node;
            if (is_callee) {
                if (index.find_function(identifier)) {
                    out_identity.category = SymbolCategory::Function;
                    out_identity.name = String(identifier);
                    return true;
                }
                if (index.find_struct(identifier)) {
                    out_identity.category = SymbolCategory::Struct;
                    out_identity.name = String(identifier);
                    return true;
                }
            }
        }

        if (parent->kind == SyntaxKind::NodeStaticGetExpr) {
            bool is_member_child = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == node;
            if (!is_member_child) {
                if (index.find_enum(identifier)) {
                    out_identity.category = SymbolCategory::Enum;
                    out_identity.name = String(identifier);
                    return true;
                }
                if (index.find_struct(identifier)) {
                    out_identity.category = SymbolCategory::Struct;
                    out_identity.name = String(identifier);
                    return true;
                }
            }
            return false;
        }

        if (parent->kind == SyntaxKind::NodeStructLiteralExpr) {
            bool is_struct_name = parent->children.size() > 0 && parent->children[0] == node;
            if (is_struct_name && index.find_struct(identifier)) {
                out_identity.category = SymbolCategory::Struct;
                out_identity.name = String(identifier);
                return true;
            }
        }

        if (parent->kind == SyntaxKind::NodeFieldInit) {
            bool is_field_name = parent->children.size() > 0 && parent->children[0] == node;
            if (is_field_name) {
                SyntaxNode* struct_literal = parent->parent;
                if (struct_literal && struct_literal->kind == SyntaxKind::NodeStructLiteralExpr) {
                    for (u32 i = 0; i < struct_literal->children.size(); i++) {
                        if (struct_literal->children[i]->kind == SyntaxKind::TokenIdentifier) {
                            StringView struct_name = struct_literal->children[i]->token.text();
                            StringView current_type = struct_name;
                            u32 depth = 0;
                            while (!current_type.empty() && depth < 16) {
                                if (index.find_field(current_type, identifier)) {
                                    out_identity.category = SymbolCategory::Field;
                                    out_identity.name = String(identifier);
                                    out_identity.qualifier = String(current_type);
                                    return true;
                                }
                                current_type = index.find_struct_parent(current_type);
                                depth++;
                            }
                            break;
                        }
                    }
                }
            }
        }

        if (parent->kind == SyntaxKind::NodeVarDecl) {
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenIdentifier &&
                    parent->children[i] == node) {
                    if (index.find_global(identifier)) {
                        out_identity.category = SymbolCategory::Global;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    SyntaxNode* enclosing_fn = find_enclosing_function(node);
                    if (enclosing_fn) {
                        out_identity.category = SymbolCategory::Local;
                        out_identity.name = String(identifier);
                        out_identity.enclosing_range = enclosing_fn->range;
                        out_identity.enclosing_uri = String(uri);
                        return true;
                    }
                    break;
                }
            }
        }

        if (parent->kind == SyntaxKind::NodeParam) {
            SyntaxNode* enclosing_fn = find_enclosing_function(node);
            if (enclosing_fn) {
                out_identity.category = SymbolCategory::Parameter;
                out_identity.name = String(identifier);
                out_identity.enclosing_range = enclosing_fn->range;
                out_identity.enclosing_uri = String(uri);
                return true;
            }
        }

        if (parent->kind == SyntaxKind::NodeFunDecl) {
            out_identity.category = SymbolCategory::Function;
            out_identity.name = String(identifier);
            return true;
        }

        if (parent->kind == SyntaxKind::NodeMethodDecl) {
            bool found_dot = false;
            StringView struct_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenDot) found_dot = true;
                if (parent->children[i] == node) {
                    if (found_dot) {
                        for (u32 j = 0; j < i; j++) {
                            if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                                struct_name = parent->children[j]->token.text();
                                break;
                            }
                        }
                        if (!struct_name.empty()) {
                            out_identity.category = SymbolCategory::Method;
                            out_identity.name = String(identifier);
                            out_identity.qualifier = String(struct_name);
                            return true;
                        }
                    } else {
                        out_identity.category = SymbolCategory::Struct;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    break;
                }
            }
        }

        if (parent->kind == SyntaxKind::NodeConstructorDecl) {
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == node) {
                    bool is_first_ident = true;
                    for (u32 j = 0; j < i; j++) {
                        if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                            is_first_ident = false;
                            break;
                        }
                    }
                    if (is_first_ident) {
                        out_identity.category = SymbolCategory::Struct;
                        out_identity.name = String(identifier);
                        return true;
                    }
                    break;
                }
            }
        }

        if (parent->kind == SyntaxKind::NodeDestructorDecl) {
            out_identity.category = SymbolCategory::Struct;
            out_identity.name = String(identifier);
            return true;
        }

        if (parent->kind == SyntaxKind::NodeStructDecl) {
            out_identity.category = SymbolCategory::Struct;
            out_identity.name = String(identifier);
            return true;
        }

        if (parent->kind == SyntaxKind::NodeEnumDecl) {
            out_identity.category = SymbolCategory::Enum;
            out_identity.name = String(identifier);
            return true;
        }

        if (parent->kind == SyntaxKind::NodeTraitDecl) {
            out_identity.category = SymbolCategory::Trait;
            out_identity.name = String(identifier);
            return true;
        }

        if (parent->kind == SyntaxKind::NodeFieldDecl) {
            SyntaxNode* struct_node = parent->parent;
            if (struct_node && struct_node->kind == SyntaxKind::NodeStructDecl) {
                for (u32 i = 0; i < struct_node->children.size(); i++) {
                    if (struct_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        out_identity.category = SymbolCategory::Field;
                        out_identity.name = String(identifier);
                        out_identity.qualifier = String(struct_node->children[i]->token.text());
                        return true;
                    }
                }
            }
        }
    }

    // General identifier — check locals first
    if (is_local_variable(node, identifier)) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        out_identity.category = SymbolCategory::Local;
        out_identity.name = String(identifier);
        out_identity.enclosing_range = enclosing_fn->range;
        out_identity.enclosing_uri = String(uri);
        return true;
    }

    if (index.find_function(identifier)) {
        out_identity.category = SymbolCategory::Function;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_struct(identifier)) {
        out_identity.category = SymbolCategory::Struct;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_enum(identifier)) {
        out_identity.category = SymbolCategory::Enum;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_trait(identifier)) {
        out_identity.category = SymbolCategory::Trait;
        out_identity.name = String(identifier);
        return true;
    }
    if (index.find_global(identifier)) {
        out_identity.category = SymbolCategory::Global;
        out_identity.name = String(identifier);
        return true;
    }
    return false;
}

// --- is_reference_to_symbol ---
static bool is_reference_to_symbol(SyntaxNode* candidate, const SymbolIdentity& target,
                                    const GlobalIndex& index, SyntaxNode* file_root,
                                    StringView file_uri,
                                    LspAnalysisContext* analysis_ctx) {
    if (!candidate) return false;

    StringView candidate_text;
    if (candidate->kind == SyntaxKind::TokenIdentifier) {
        candidate_text = candidate->token.text();
    } else if (candidate->kind == SyntaxKind::TokenKwSelf) {
        candidate_text = StringView("self");
    } else {
        return false;
    }

    SyntaxNode* parent = candidate->parent;

    switch (target.category) {
    case SymbolCategory::Function: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }
        if (parent->kind == SyntaxKind::NodeTypeExpr) return false;
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;
        if (parent->kind == SyntaxKind::NodeStructDecl ||
            parent->kind == SyntaxKind::NodeEnumDecl ||
            parent->kind == SyntaxKind::NodeTraitDecl) return false;

        if (is_local_variable(candidate, candidate_text)) return false;

        if (parent->kind == SyntaxKind::NodeFunDecl) return true;
        return index.find_function(candidate_text) != nullptr;
    }

    case SymbolCategory::Struct: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;
        if (parent->kind == SyntaxKind::NodeParam) {
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == candidate) {
                    if (i + 1 < parent->children.size() &&
                        parent->children[i + 1]->kind == SyntaxKind::TokenColon) {
                        return false;
                    }
                    break;
                }
            }
        }
        if (is_local_variable(candidate, candidate_text)) return false;
        return true;
    }

    case SymbolCategory::Enum: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;
        if (is_local_variable(candidate, candidate_text)) return false;
        return true;
    }

    case SymbolCategory::Trait: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }
        return true;
    }

    case SymbolCategory::Global: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (is_member_name) return false;
        }
        if (parent->kind == SyntaxKind::NodeTypeExpr) return false;
        if (parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent->kind == SyntaxKind::NodeFieldDecl) return false;
        if (parent->kind == SyntaxKind::NodeStructDecl ||
            parent->kind == SyntaxKind::NodeEnumDecl ||
            parent->kind == SyntaxKind::NodeTraitDecl ||
            parent->kind == SyntaxKind::NodeFunDecl) return false;
        if (is_local_variable(candidate, candidate_text)) return false;
        return true;
    }

    case SymbolCategory::Method: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (!is_member_name) return false;
            SyntaxNode* object_expr = parent->children[0];
            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            String receiver_type = resolve_receiver_type(object_expr, enclosing_fn, analysis_ctx);
            if (receiver_type.empty()) return false;
            StringView current_type(receiver_type.data(), receiver_type.size());
            u32 depth = 0;
            while (!current_type.empty() && depth < 16) {
                if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                    index.find_method(current_type, candidate_text)) {
                    return true;
                }
                current_type = index.find_struct_parent(current_type);
                depth++;
            }
            return false;
        }
        if (parent->kind == SyntaxKind::NodeMethodDecl) {
            bool found_dot = false;
            StringView struct_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenDot) found_dot = true;
                if (parent->children[i] == candidate) {
                    if (found_dot) {
                        for (u32 j = 0; j < i; j++) {
                            if (parent->children[j]->kind == SyntaxKind::TokenIdentifier) {
                                struct_name = parent->children[j]->token.text();
                                break;
                            }
                        }
                        return struct_name == StringView(target.qualifier.data(), target.qualifier.size());
                    }
                    break;
                }
            }
        }
        return false;
    }

    case SymbolCategory::Field: {
        if (!parent) return false;
        if (parent->kind == SyntaxKind::NodeGetExpr) {
            bool is_member_name = parent->children.size() >= 3 &&
                parent->children[parent->children.size() - 1] == candidate;
            if (!is_member_name) return false;
            SyntaxNode* object_expr = parent->children[0];
            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            String receiver_type = resolve_receiver_type(object_expr, enclosing_fn, analysis_ctx);
            if (receiver_type.empty()) return false;
            StringView current_type(receiver_type.data(), receiver_type.size());
            u32 depth = 0;
            while (!current_type.empty() && depth < 16) {
                if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                    index.find_field(current_type, candidate_text)) {
                    return true;
                }
                current_type = index.find_struct_parent(current_type);
                depth++;
            }
            return false;
        }
        if (parent->kind == SyntaxKind::NodeFieldDecl) {
            bool is_name = false;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i] == candidate) { is_name = true; break; }
                if (parent->children[i]->kind == SyntaxKind::TokenColon) break;
            }
            if (!is_name) return false;
            SyntaxNode* struct_node = parent->parent;
            if (struct_node && struct_node->kind == SyntaxKind::NodeStructDecl) {
                for (u32 i = 0; i < struct_node->children.size(); i++) {
                    if (struct_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        StringView struct_name = struct_node->children[i]->token.text();
                        return struct_name == StringView(target.qualifier.data(), target.qualifier.size());
                    }
                }
            }
            return false;
        }
        if (parent->kind == SyntaxKind::NodeFieldInit) {
            bool is_field_name = parent->children.size() > 0 && parent->children[0] == candidate;
            if (!is_field_name) return false;
            SyntaxNode* struct_literal = parent->parent;
            if (struct_literal && struct_literal->kind == SyntaxKind::NodeStructLiteralExpr) {
                for (u32 i = 0; i < struct_literal->children.size(); i++) {
                    if (struct_literal->children[i]->kind == SyntaxKind::TokenIdentifier) {
                        StringView struct_name = struct_literal->children[i]->token.text();
                        StringView current_type = struct_name;
                        u32 depth = 0;
                        while (!current_type.empty() && depth < 16) {
                            if (current_type == StringView(target.qualifier.data(), target.qualifier.size()) &&
                                index.find_field(current_type, candidate_text)) {
                                return true;
                            }
                            current_type = index.find_struct_parent(current_type);
                            depth++;
                        }
                        break;
                    }
                }
            }
            return false;
        }
        return false;
    }

    case SymbolCategory::Local:
    case SymbolCategory::Parameter: {
        if (file_uri != StringView(target.enclosing_uri.data(), target.enclosing_uri.size())) {
            return false;
        }
        SyntaxNode* enclosing_fn = find_enclosing_function(candidate);
        if (!enclosing_fn) return false;
        if (enclosing_fn->range.start != target.enclosing_range.start ||
            enclosing_fn->range.end != target.enclosing_range.end) {
            return false;
        }
        if (parent && parent->kind == SyntaxKind::NodeTypeExpr) return false;
        if (parent && parent->kind == SyntaxKind::NodeFieldInit &&
            parent->children.size() > 0 && parent->children[0] == candidate) return false;
        if (parent && parent->kind == SyntaxKind::NodeFieldDecl) return false;
        if (parent && (parent->kind == SyntaxKind::NodeStructDecl ||
                       parent->kind == SyntaxKind::NodeEnumDecl ||
                       parent->kind == SyntaxKind::NodeTraitDecl ||
                       parent->kind == SyntaxKind::NodeFunDecl)) return false;
        return true;
    }

    case SymbolCategory::Constructor:
        return false;
    }

    return false;
}

// Helper: find references using the test infrastructure
struct ReferenceResult {
    u32 count;
    Vector<TextRange> ranges;
};

static ReferenceResult find_references(const char* source, u32 cursor_offset,
                                        bool include_declaration = true) {
    u32 length = static_cast<u32>(strlen(source));
    String uri("file:///test.roxy");

    // Build GlobalIndex
    BumpAllocator index_allocator(8192);
    Lexer index_lexer(source, length);
    LspParser index_parser(index_lexer, index_allocator);
    SyntaxTree index_tree = index_parser.parse();
    FileIndexer indexer;
    FileStubs stubs = indexer.index(index_tree.root);
    GlobalIndex index;
    index.update_file(uri, stubs);

    // Build LspAnalysisContext
    LspAnalysisContext analysis_ctx;
    LspAnalysisContext::SourceFile src_file;
    src_file.uri = StringView("file:///test.roxy");
    src_file.source = source;
    src_file.source_length = length;
    src_file.cst_root = index_tree.root;
    analysis_ctx.rebuild_declarations(Span<LspAnalysisContext::SourceFile>(&src_file, 1));

    // Parse CST for cursor lookup
    BumpAllocator allocator(8192);
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    SyntaxNode* node = find_node_at_offset(tree.root, cursor_offset);
    if (!node) return {0, {}};

    SymbolIdentity identity;
    if (!identify_symbol_at_cursor(node, index, StringView(uri.data(), uri.size()), identity,
                                    &analysis_ctx)) {
        return {0, {}};
    }

    // Collect and filter references
    StringView target_name(identity.name.data(), identity.name.size());
    Vector<SyntaxNode*> candidates;
    collect_identifiers(tree.root, target_name, candidates);

    ReferenceResult result;
    result.count = 0;

    for (u32 i = 0; i < candidates.size(); i++) {
        if (is_reference_to_symbol(candidates[i], identity, index, tree.root,
                                    StringView(uri.data(), uri.size()), &analysis_ctx)) {
            // Check declaration filtering
            if (!include_declaration) {
                SyntaxNode* parent = candidates[i]->parent;
                bool is_decl = false;
                if (parent) {
                    switch (identity.category) {
                    case SymbolCategory::Function:
                        is_decl = (parent->kind == SyntaxKind::NodeFunDecl);
                        break;
                    case SymbolCategory::Struct:
                        is_decl = (parent->kind == SyntaxKind::NodeStructDecl);
                        break;
                    case SymbolCategory::Enum:
                        is_decl = (parent->kind == SyntaxKind::NodeEnumDecl);
                        break;
                    case SymbolCategory::Trait:
                        is_decl = (parent->kind == SyntaxKind::NodeTraitDecl);
                        break;
                    case SymbolCategory::Global:
                        is_decl = (parent->kind == SyntaxKind::NodeVarDecl &&
                                   find_enclosing_function(candidates[i]) == nullptr);
                        break;
                    case SymbolCategory::Method:
                        is_decl = (parent->kind == SyntaxKind::NodeMethodDecl);
                        break;
                    case SymbolCategory::Field:
                        is_decl = (parent->kind == SyntaxKind::NodeFieldDecl);
                        break;
                    case SymbolCategory::Local:
                        is_decl = (parent->kind == SyntaxKind::NodeVarDecl);
                        break;
                    case SymbolCategory::Parameter:
                        is_decl = (parent->kind == SyntaxKind::NodeParam);
                        break;
                    case SymbolCategory::Constructor:
                        is_decl = (parent->kind == SyntaxKind::NodeConstructorDecl);
                        break;
                    }
                }
                if (is_decl) continue;
            }

            result.ranges.push_back(candidates[i]->range);
            result.count++;
        }
    }

    return result;
}

// ============================================================
// Test cases
// ============================================================

// --- Function references ---

TEST_CASE("References - function: definition + 2 call sites") {
    const char* source =
        "fun add(a: i32, b: i32): i32 { return a + b; }\n"
        "fun main() {\n"
        "    var x = add(1, 2);\n"
        "    var y = add(3, 4);\n"
        "}\n";

    // Cursor on "add" in the function definition (offset 4)
    auto result = find_references(source, 4);
    CHECK(result.count == 3);  // definition + 2 call sites
}

TEST_CASE("References - function: no call sites (definition only)") {
    const char* source =
        "fun unused(): i32 { return 0; }\n"
        "fun main() {\n"
        "    var x = 1;\n"
        "}\n";

    // Cursor on "unused" (offset 4)
    auto result = find_references(source, 4);
    CHECK(result.count == 1);  // definition only
}

TEST_CASE("References - function: rename add -> sum") {
    const char* source =
        "fun add(a: i32, b: i32): i32 { return a + b; }\n"
        "fun main() {\n"
        "    var x = add(1, 2);\n"
        "    var y = add(3, 4);\n"
        "}\n";

    // Rename from a call site
    u32 call_offset = static_cast<u32>(strstr(source, "add(1") - source);
    auto result = find_references(source, call_offset);
    CHECK(result.count == 3);  // All occurrences should be found
}

// --- Type references ---

TEST_CASE("References - struct: definition + type annotations + struct literal") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun make_point(): Point {\n"
        "    return Point { x = 1.0, y = 2.0 };\n"
        "}\n"
        "fun use_point(p: Point) {\n"
        "}\n";

    // Cursor on "Point" in struct definition (offset 7)
    auto result = find_references(source, 7);
    CHECK(result.count == 4);  // definition + return type + struct literal + param type
}

TEST_CASE("References - enum: definition + static access") {
    const char* source =
        "enum Color { Red, Green, Blue }\n"
        "fun get_color(): Color {\n"
        "    return Color::Red;\n"
        "}\n";

    // Cursor on "Color" in enum definition (offset 5)
    auto result = find_references(source, 5);
    CHECK(result.count == 3);  // definition + return type + Color::Red
}

TEST_CASE("References - struct: rename Point -> Vec2") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun make(): Point {\n"
        "    return Point { x = 1.0, y = 2.0 };\n"
        "}\n";

    // Cursor on "Point" in struct definition
    auto result = find_references(source, 7);
    CHECK(result.count == 3);  // definition + return type + struct literal
}

// --- Field/method references ---

TEST_CASE("References - field: definition + access sites") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p: Point = Point { x = 1.0, y = 2.0 };\n"
        "    var a = p.x;\n"
        "    var b = p.x;\n"
        "}\n";

    // Cursor on "x" in field definition
    // "struct Point { x: f32" — "x" is at offset 15
    auto result = find_references(source, 15);
    // definition + struct literal "x" + p.x + p.x = 4
    CHECK(result.count == 4);
}

TEST_CASE("References - method: definition + call sites") {
    const char* source =
        "struct Point { x: f32; }\n"
        "fun Point.length(): f32 { return self.x; }\n"
        "fun test() {\n"
        "    var p: Point = Point { x = 1.0 };\n"
        "    var a = p.length();\n"
        "    var b = p.length();\n"
        "}\n";

    // Cursor on "length" in method definition
    u32 length_offset = static_cast<u32>(strstr(source, "length") - source);
    auto result = find_references(source, length_offset);
    CHECK(result.count == 3);  // definition + 2 call sites
}

TEST_CASE("References - field: same name on different structs") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "struct Size { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p: Point = Point { x = 1.0, y = 2.0 };\n"
        "    var s: Size = Size { x = 10.0, y = 20.0 };\n"
        "    var a = p.x;\n"
        "    var b = s.x;\n"
        "}\n";

    // Cursor on "x" field of Point (offset 15)
    auto result = find_references(source, 15);
    // Point.x definition + Point struct literal "x" + p.x = 3
    // Should NOT include Size.x or s.x
    CHECK(result.count == 3);
}

// --- Local variable references ---

TEST_CASE("References - local: var decl + usages within function") {
    const char* source =
        "fun test() {\n"
        "    var x = 10;\n"
        "    var y = x + 1;\n"
        "    var z = x + 2;\n"
        "}\n";

    // Cursor on "x" in "var x = 10" — find "x"
    u32 x_offset = static_cast<u32>(strstr(source, "x = 10") - source);
    auto result = find_references(source, x_offset);
    CHECK(result.count == 3);  // decl + 2 usages
}

TEST_CASE("References - local: same name in different functions") {
    const char* source =
        "fun foo() {\n"
        "    var x = 10;\n"
        "    var y = x + 1;\n"
        "}\n"
        "fun bar() {\n"
        "    var x = 20;\n"
        "    var z = x + 2;\n"
        "}\n";

    // Cursor on "x" in foo
    u32 x_offset = static_cast<u32>(strstr(source, "x = 10") - source);
    auto result = find_references(source, x_offset);
    CHECK(result.count == 2);  // Only "var x = 10" and "x + 1" in foo
}

TEST_CASE("References - parameter: find all usages in function body") {
    const char* source =
        "fun add(a: i32, b: i32): i32 {\n"
        "    return a + b;\n"
        "}\n";

    // Cursor on "a" in param list
    u32 a_offset = static_cast<u32>(strstr(source, "a: i32") - source);
    auto result = find_references(source, a_offset);
    CHECK(result.count == 2);  // param decl + usage in return
}

// --- Edge cases ---

TEST_CASE("References - cursor on definition vs usage gives same results") {
    const char* source =
        "fun greet() {}\n"
        "fun main() { greet(); }\n";

    // From definition
    auto result_from_def = find_references(source, 4);  // "greet" in fun greet
    // From usage
    u32 usage_offset = static_cast<u32>(strstr(source, "greet()") + 6 - source);  // after first greet
    // Actually find second "greet" occurrence
    const char* second = strstr(source + 15, "greet");
    u32 call_offset = static_cast<u32>(second - source);
    auto result_from_usage = find_references(source, call_offset);

    CHECK(result_from_def.count == result_from_usage.count);
}

TEST_CASE("References - includeDeclaration=false excludes definition") {
    const char* source =
        "fun add(a: i32, b: i32): i32 { return a + b; }\n"
        "fun main() {\n"
        "    var x = add(1, 2);\n"
        "}\n";

    auto result_with_decl = find_references(source, 4, true);
    auto result_without_decl = find_references(source, 4, false);
    CHECK(result_with_decl.count == result_without_decl.count + 1);
}

TEST_CASE("References - rename with no references (definition only)") {
    const char* source =
        "fun standalone(): i32 { return 42; }\n";

    auto result = find_references(source, 4);
    CHECK(result.count == 1);  // Only the definition
}

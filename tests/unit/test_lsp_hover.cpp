#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/global_index.hpp"
#include "roxy/lsp/lsp_analysis_context.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_map.h"

#include <cstring>

using namespace rx;

// Bundles GlobalIndex + LspAnalysisContext for tests
struct HoverTestContext {
    BumpAllocator index_alloc{8192};
    GlobalIndex index;
    LspAnalysisContext analysis_ctx;

    void setup(const char* source) {
        u32 length = static_cast<u32>(strlen(source));
        Lexer lexer(source, length);
        LspParser parser(lexer, index_alloc);
        SyntaxTree tree = parser.parse();

        FileIndexer indexer;
        FileStubs stubs = indexer.index(tree.root);
        String uri("file:///test.roxy");
        index.update_file(uri, stubs);

        LspAnalysisContext::SourceFile file;
        file.uri = StringView("file:///test.roxy");
        file.source = source;
        file.source_length = length;
        file.cst_root = tree.root;
        analysis_ctx.rebuild_declarations(Span<LspAnalysisContext::SourceFile>(&file, 1));
    }
};

// Helper: find the SyntaxNode at a given byte offset
static SyntaxNode* parse_and_find(const char* source, u32 offset, BumpAllocator& allocator,
                                   SyntaxTree& out_tree) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    out_tree = parser.parse();
    return find_node_at_offset(out_tree.root, offset);
}

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

// Resolve hover text for a node — mirrors the logic in handle_hover
static String resolve_hover(SyntaxNode* node, const char* source, u32 source_length,
                             GlobalIndex& index, LspAnalysisContext& analysis_ctx) {
    if (!node) return String();

    // Bool literals
    if (node->kind == SyntaxKind::TokenKwTrue || node->kind == SyntaxKind::TokenKwFalse) {
        return String("bool");
    }

    // Nil
    if (node->kind == SyntaxKind::TokenKwNil) {
        return String("nil");
    }

    // Self keyword
    if (node->kind == SyntaxKind::TokenKwSelf) {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (enclosing_fn) {
            SyntaxNode* struct_ident = nullptr;
            for (u32 i = 0; i < enclosing_fn->children.size(); i++) {
                if (enclosing_fn->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    struct_ident = enclosing_fn->children[i];
                    break;
                }
            }
            if (struct_ident) {
                String result("self: ");
                result.append(struct_ident->token.text().data(), struct_ident->token.text().size());
                return result;
            }
        }
        return String();
    }

    // Only identifiers from here on
    if (node->kind != SyntaxKind::TokenIdentifier) return String();

    StringView identifier = node->token.text();
    SyntaxNode* parent = node->parent;

    // Field/method access (NodeGetExpr)
    if (parent && parent->kind == SyntaxKind::NodeGetExpr) {
        bool is_member_name = parent->children.size() >= 3 &&
            parent->children[parent->children.size() - 1] == node;

        if (is_member_name) {
            SyntaxNode* object_expr = parent->children[0];
            String receiver_type;

            SyntaxNode* enclosing_fn = find_enclosing_function(parent);
            if (enclosing_fn) {
                BumpAllocator ast_allocator(8192);
                BodyAnalysisResult body_result = analysis_ctx.analyze_function_body(
                    enclosing_fn, ast_allocator);
                if (body_result.decl) {
                    tsl::robin_map<String, Type*> local_vars;
                    analysis_ctx.collect_local_variables(body_result.decl, local_vars);
                    Type* recv_type = analysis_ctx.resolve_cst_expr_type(object_expr, local_vars);
                    if (recv_type && !recv_type->is_error()) {
                        receiver_type = LspAnalysisContext::type_to_string(recv_type);
                    }
                }
            }

            if (!receiver_type.empty()) {
                StringView current_type(receiver_type.data(), receiver_type.size());
                u32 depth = 0;
                while (!current_type.empty() && depth < 16) {
                    StringView field_type = index.find_field_type(current_type, identifier);
                    if (!field_type.empty()) {
                        String result("(field) ");
                        result.append(current_type.data(), current_type.size());
                        result.push_back('.');
                        result.append(identifier.data(), identifier.size());
                        result.append(": ", 2);
                        result.append(field_type.data(), field_type.size());
                        return result;
                    }

                    StringView method_sig = index.find_method_signature(current_type, identifier);
                    if (!method_sig.empty()) {
                        String result("fun ");
                        result.append(current_type.data(), current_type.size());
                        result.push_back('.');
                        result.append(identifier.data(), identifier.size());
                        result.append(method_sig.data(), method_sig.size());
                        return result;
                    }

                    current_type = index.find_struct_parent(current_type);
                    depth++;
                }
            }
            return String();
        }
    }

    // Static access (NodeStaticGetExpr)
    if (parent && parent->kind == SyntaxKind::NodeStaticGetExpr) {
        bool is_member_child = parent->children.size() >= 3 &&
            parent->children[parent->children.size() - 1] == node;

        if (is_member_child) {
            StringView type_name;
            for (u32 i = 0; i < parent->children.size(); i++) {
                if (parent->children[i]->kind == SyntaxKind::TokenIdentifier &&
                    parent->children[i] != node) {
                    type_name = parent->children[i]->token.text();
                    break;
                }
            }
            if (!type_name.empty()) {
                String result("(variant) ");
                result.append(type_name.data(), type_name.size());
                result.append("::", 2);
                result.append(identifier.data(), identifier.size());
                return result;
            }
        } else {
            if (index.find_enum(identifier)) {
                String result("enum ");
                result.append(identifier.data(), identifier.size());
                return result;
            }
            if (index.find_struct(identifier)) {
                String result("struct ");
                result.append(identifier.data(), identifier.size());
                return result;
            }
        }
        return String();
    }

    // Type annotation (NodeTypeExpr)
    if (parent && parent->kind == SyntaxKind::NodeTypeExpr) {
        if (index.find_struct(identifier)) {
            String result("struct ");
            result.append(identifier.data(), identifier.size());
            return result;
        }
        if (index.find_enum(identifier)) {
            String result("enum ");
            result.append(identifier.data(), identifier.size());
            return result;
        }
        if (index.find_trait(identifier)) {
            String result("trait ");
            result.append(identifier.data(), identifier.size());
            return result;
        }
        return String();
    }

    // Function call (NodeCallExpr)
    if (parent && parent->kind == SyntaxKind::NodeCallExpr) {
        bool is_callee = parent->children.size() > 0 && parent->children[0] == node;
        if (is_callee) {
            StringView func_sig = index.find_function_signature(identifier);
            if (!func_sig.empty()) {
                String result("fun ");
                result.append(identifier.data(), identifier.size());
                result.append(func_sig.data(), func_sig.size());
                return result;
            }
        }
    }

    // Struct literal (NodeStructLiteralExpr)
    if (parent && parent->kind == SyntaxKind::NodeStructLiteralExpr) {
        bool is_struct_name = parent->children.size() > 0 && parent->children[0] == node;
        if (is_struct_name && index.find_struct(identifier)) {
            String result("struct ");
            result.append(identifier.data(), identifier.size());
            return result;
        }
    }

    // Field initializer (NodeFieldInit)
    if (parent && parent->kind == SyntaxKind::NodeFieldInit) {
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
                            StringView field_type = index.find_field_type(current_type, identifier);
                            if (!field_type.empty()) {
                                String result("(field) ");
                                result.append(current_type.data(), current_type.size());
                                result.push_back('.');
                                result.append(identifier.data(), identifier.size());
                                result.append(": ", 2);
                                result.append(field_type.data(), field_type.size());
                                return result;
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

    // Local variable resolution
    {
        SyntaxNode* enclosing_fn = find_enclosing_function(node);
        if (enclosing_fn) {
            BumpAllocator ast_allocator(8192);
            BodyAnalysisResult body_result = analysis_ctx.analyze_function_body(
                enclosing_fn, ast_allocator);
            if (body_result.decl) {
                tsl::robin_map<String, Type*> local_vars;
                analysis_ctx.collect_local_variables(body_result.decl, local_vars);
                auto var_it = local_vars.find(String(identifier));
                if (var_it != local_vars.end()) {
                    String type_str = LspAnalysisContext::type_to_string(var_it->second);
                    String result("(variable) ");
                    result.append(identifier.data(), identifier.size());
                    result.append(": ", 2);
                    result.append(type_str.data(), type_str.size());
                    return result;
                }
            }
        }
    }

    // Fallback: GlobalIndex lookup
    if (index.find_function(identifier)) {
        StringView func_sig = index.find_function_signature(identifier);
        if (!func_sig.empty()) {
            String result("fun ");
            result.append(identifier.data(), identifier.size());
            result.append(func_sig.data(), func_sig.size());
            return result;
        }
    }
    if (index.find_struct(identifier)) {
        String result("struct ");
        result.append(identifier.data(), identifier.size());
        return result;
    }
    if (index.find_enum(identifier)) {
        String result("enum ");
        result.append(identifier.data(), identifier.size());
        return result;
    }
    if (index.find_trait(identifier)) {
        String result("trait ");
        result.append(identifier.data(), identifier.size());
        return result;
    }
    if (index.find_global(identifier)) {
        StringView global_type = index.find_global_type(identifier);
        String result("(global) ");
        result.append(identifier.data(), identifier.size());
        if (!global_type.empty()) {
            result.append(": ", 2);
            result.append(global_type.data(), global_type.size());
        }
        return result;
    }

    return String();
}

// --- Hover tests ---

TEST_CASE("Hover - Variable in expression") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p: Point;\n"
        "    p;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find the 'p' in the expression statement "p;"
    // "    p;" — find the offset of the second 'p'
    u32 offset = static_cast<u32>(strstr(source + 50, "p;") - source);
    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(variable) p: Point"));
}

TEST_CASE("Hover - Function name") {
    const char* source = "fun add(a: i32, b: i32): i32 { return a + b; }";

    HoverTestContext ctx;
    ctx.setup(source);

    // Hover on "add" — offset at 'a' of 'add'
    u32 offset = 4;
    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("fun add(a: i32, b: i32): i32"));
}

TEST_CASE("Hover - Method after dot") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun Point.length(): f32 { return 0.0; }\n"
        "fun test() {\n"
        "    var p: Point;\n"
        "    p.length();\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "length" after "p." in "p.length();"
    const char* length_pos = strstr(source, "p.length()");
    REQUIRE(length_pos != nullptr);
    u32 offset = static_cast<u32>(length_pos - source) + 2; // skip "p." to hit "length"

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("fun Point.length(): f32"));
}

TEST_CASE("Hover - Field after dot") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p: Point;\n"
        "    p.x;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "x" after "p." in "p.x;"
    const char* access_pos = strstr(source, "p.x;");
    REQUIRE(access_pos != nullptr);
    u32 offset = static_cast<u32>(access_pos - source) + 2; // skip "p." to hit "x"

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(field) Point.x: f32"));
}

TEST_CASE("Hover - Struct type in annotation") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p: Point;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "Point" in "var p: Point;"
    const char* type_pos = strstr(source + 33, "Point");
    REQUIRE(type_pos != nullptr);
    u32 offset = static_cast<u32>(type_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("struct Point"));
}

TEST_CASE("Hover - Enum type") {
    const char* source =
        "enum Color { Red, Green, Blue }\n"
        "fun test() {\n"
        "    var c: Color;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "Color" in "var c: Color;"
    const char* type_pos = strstr(source + 31, "Color");
    REQUIRE(type_pos != nullptr);
    u32 offset = static_cast<u32>(type_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("enum Color"));
}

TEST_CASE("Hover - Self keyword") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun Point.length(): f32 {\n"
        "    return self.x;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "self" in "self.x"
    const char* self_pos = strstr(source, "self");
    REQUIRE(self_pos != nullptr);
    u32 offset = static_cast<u32>(self_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("self: Point"));
}

TEST_CASE("Hover - Enum variant via static access") {
    const char* source =
        "enum Color { Red, Green, Blue }\n"
        "fun test() {\n"
        "    var c = Color::Red;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "Red" in "Color::Red"
    const char* red_pos = strstr(source, "Red;");
    REQUIRE(red_pos != nullptr);
    u32 offset = static_cast<u32>(red_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(variant) Color::Red"));
}

TEST_CASE("Hover - Inherited field") {
    const char* source =
        "struct Base { x: i32; }\n"
        "struct Child : Base { y: i32; }\n"
        "fun test() {\n"
        "    var c: Child;\n"
        "    c.x;\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "x" in "c.x;"
    const char* access_pos = strstr(source, "c.x;");
    REQUIRE(access_pos != nullptr);
    u32 offset = static_cast<u32>(access_pos - source) + 2; // skip "c." to "x"

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(field) Base.x: i32"));
}

TEST_CASE("Hover - Parameter in function body") {
    const char* source = "fun add(a: i32, b: i32): i32 { return a + b; }";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find 'a' inside the body (after "return ")
    const char* a_pos = strstr(source, "return a");
    REQUIRE(a_pos != nullptr);
    u32 offset = static_cast<u32>(a_pos - source) + 7; // "return " -> 'a'

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(variable) a: i32"));
}

TEST_CASE("Hover - Bool literal true") {
    const char* source = "fun test() { var b = true; }";

    HoverTestContext ctx;
    ctx.setup(source);

    const char* true_pos = strstr(source, "true");
    REQUIRE(true_pos != nullptr);
    u32 offset = static_cast<u32>(true_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("bool"));
}

TEST_CASE("Hover - Struct literal name") {
    const char* source =
        "struct Point { x: f32; y: f32; }\n"
        "fun test() {\n"
        "    var p = Point { x = 1.0, y = 2.0 };\n"
        "}\n";

    HoverTestContext ctx;
    ctx.setup(source);

    // Find "Point" in "Point { x = 1.0"
    const char* lit_pos = strstr(source + 33, "Point");
    REQUIRE(lit_pos != nullptr);
    u32 offset = static_cast<u32>(lit_pos - source);

    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("struct Point"));
}

TEST_CASE("Hover - Global variable") {
    const char* source = "var count: i32 = 0;";

    HoverTestContext ctx;
    ctx.setup(source);

    // Hover on "count" — offset at 'c' of 'count'
    u32 offset = 4;
    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);
    REQUIRE(node != nullptr);

    String hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                                  ctx.index, ctx.analysis_ctx);
    CHECK(hover == String("(global) count: i32"));
}

TEST_CASE("Hover - Unresolvable (whitespace)") {
    const char* source = "fun test() { var x: i32; }";

    HoverTestContext ctx;
    ctx.setup(source);

    // Try offset in whitespace before 'var' — should return empty
    u32 offset = 13; // space after '{'
    BumpAllocator parse_allocator(8192);
    SyntaxTree tree;
    SyntaxNode* node = parse_and_find(source, offset, parse_allocator, tree);

    // Node may be a keyword or non-identifier; hover should return empty
    String hover;
    if (node) {
        hover = resolve_hover(node, source, static_cast<u32>(strlen(source)),
                               ctx.index, ctx.analysis_ctx);
    }
    CHECK(hover.empty());
}

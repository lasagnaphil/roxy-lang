#include "roxy/lsp/lsp_analysis_context.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/natives.hpp"

#include <cstring>

namespace rx {

LspAnalysisContext::LspAnalysisContext()
    : m_type_allocator(new BumpAllocator(32768))
    , m_type_env(new TypeEnv(*m_type_allocator))
    , m_module_registry(new ModuleRegistry(*m_type_allocator))
    , m_builtin_registry(new NativeRegistry(*m_type_allocator, m_type_env->types()))
    , m_decl_symbols(nullptr)
{
    init_builtins();
}

LspAnalysisContext::~LspAnalysisContext() = default;

void LspAnalysisContext::init_builtins() {
    // Register built-in natives and add as "builtin" module (auto-imported as prelude)
    register_builtin_natives(*m_builtin_registry);
    m_module_registry->register_native_module(
        BUILTIN_MODULE_NAME, m_builtin_registry.get(), m_type_env->types());
}

void LspAnalysisContext::rebuild_declarations(Span<SourceFile> files) {
    // Reset all state for a full rebuild using a single allocator
    // (mirrors the compiler's single-allocator pattern)
    m_type_allocator.reset();
    m_type_allocator = UniquePtr<BumpAllocator>(new BumpAllocator(32768));
    m_type_env.reset();
    m_type_env = UniquePtr<TypeEnv>(new TypeEnv(*m_type_allocator));
    m_module_registry.reset();
    m_module_registry = UniquePtr<ModuleRegistry>(new ModuleRegistry(*m_type_allocator));
    m_builtin_registry.reset();
    m_builtin_registry = UniquePtr<NativeRegistry>(new NativeRegistry(*m_type_allocator, m_type_env->types()));
    init_builtins();

    // Lower all CSTs to a combined AST program (using the same allocator)
    CstLowering lowering(*m_type_allocator);

    // Collect all declarations from all files into a single program
    Vector<Decl*> all_declarations;
    for (u32 i = 0; i < files.size(); i++) {
        const SourceFile& file = files[i];
        if (!file.cst_root) continue;

        Program* file_program = lowering.lower(file.cst_root);
        if (!file_program) continue;

        for (auto* decl : file_program->declarations) {
            if (decl) {
                all_declarations.push_back(decl);
            }
        }
    }

    // Build combined program
    auto* program = m_type_allocator->emplace<Program>();
    program->module_name = StringView("", 0);  // LSP uses a single unnamed module
    if (!all_declarations.empty()) {
        Decl** decl_data = reinterpret_cast<Decl**>(
            m_type_allocator->alloc_bytes(
                sizeof(Decl*) * all_declarations.size(), alignof(Decl*)));
        for (u32 i = 0; i < all_declarations.size(); i++) {
            decl_data[i] = all_declarations[i];
        }
        program->declarations = Span<Decl*>(decl_data, static_cast<u32>(all_declarations.size()));
    } else {
        program->declarations = Span<Decl*>(nullptr, 0);
    }

    // Create symbol table for declaration passes
    m_decl_symbols.reset();
    m_decl_symbols = UniquePtr<SymbolTable>(new SymbolTable(*m_type_allocator));

    // Run declaration passes (0-2) using SemanticAnalyzer in LSP mode
    SemanticAnalyzer analyzer(*m_type_allocator, *m_type_env, *m_module_registry,
                              *m_decl_symbols, nullptr);
    analyzer.set_lsp_mode(true);
    analyzer.run_declaration_passes(program);

    m_decl_program = program;
    m_declaration_version++;
    m_initialized = true;
}

BodyAnalysisResult LspAnalysisContext::analyze_function_body(
    SyntaxNode* fn_cst, BumpAllocator& ast_allocator)
{
    BodyAnalysisResult result;
    if (!fn_cst || !m_initialized) return result;

    // Lower the CST function node to AST
    CstLowering lowering(ast_allocator);
    Decl* ast_decl = lowering.lower_decl(fn_cst);
    if (!ast_decl) return result;

    result.decl = ast_decl;

    // Create a fresh symbol table that inherits from the declaration-level symbols
    auto* body_symbols = new SymbolTable(*m_type_allocator);

    // Copy declaration-level symbols into the new table
    // We re-run a minimal SemanticAnalyzer that shares the populated TypeEnv
    SemanticAnalyzer analyzer(ast_allocator, *m_type_env, *m_module_registry,
                              *body_symbols, nullptr);
    analyzer.set_lsp_mode(true);

    // Import builtin prelude and declaration-level symbols
    // Then analyze just this one function body
    analyzer.analyze_single_function(ast_decl);

    result.symbols = body_symbols;
    result.errors = analyzer.errors();
    result.success = !analyzer.has_errors();

    return result;
}

String LspAnalysisContext::type_to_string(Type* type) {
    if (!type) return String("unknown");

    switch (type->kind) {
        case TypeKind::Void:    return String("void");
        case TypeKind::Bool:    return String("bool");
        case TypeKind::I8:      return String("i8");
        case TypeKind::I16:     return String("i16");
        case TypeKind::I32:     return String("i32");
        case TypeKind::I64:     return String("i64");
        case TypeKind::U8:      return String("u8");
        case TypeKind::U16:     return String("u16");
        case TypeKind::U32:     return String("u32");
        case TypeKind::U64:     return String("u64");
        case TypeKind::F32:     return String("f32");
        case TypeKind::F64:     return String("f64");
        case TypeKind::String:  return String("string");
        case TypeKind::Nil:     return String("nil");
        case TypeKind::Error:   return String("<error>");
        case TypeKind::IntLiteral: return String("int");

        case TypeKind::Struct: {
            String result(type->struct_info.name.data(), type->struct_info.name.size());
            return result;
        }

        case TypeKind::Enum: {
            String result(type->enum_info.name.data(), type->enum_info.name.size());
            return result;
        }

        case TypeKind::Trait: {
            String result(type->trait_info.name.data(), type->trait_info.name.size());
            return result;
        }

        case TypeKind::List: {
            String result("List<");
            String elem = type_to_string(type->list_info.element_type);
            result.append(elem.data(), elem.size());
            result.push_back('>');
            return result;
        }

        case TypeKind::Map: {
            String result("Map<");
            String key = type_to_string(type->map_info.key_type);
            result.append(key.data(), key.size());
            result.append(", ", 2);
            String val = type_to_string(type->map_info.value_type);
            result.append(val.data(), val.size());
            result.push_back('>');
            return result;
        }

        case TypeKind::Coroutine: {
            String result("Coro<");
            String yield = type_to_string(type->coro_info.yield_type);
            result.append(yield.data(), yield.size());
            result.push_back('>');
            return result;
        }

        case TypeKind::Uniq: {
            String result("uniq ");
            String inner = type_to_string(type->ref_info.inner_type);
            result.append(inner.data(), inner.size());
            return result;
        }

        case TypeKind::Ref: {
            String result("ref ");
            String inner = type_to_string(type->ref_info.inner_type);
            result.append(inner.data(), inner.size());
            return result;
        }

        case TypeKind::Weak: {
            String result("weak ");
            String inner = type_to_string(type->ref_info.inner_type);
            result.append(inner.data(), inner.size());
            return result;
        }

        case TypeKind::Function: {
            String result("fun(");
            for (u32 i = 0; i < type->func_info.param_types.size(); i++) {
                if (i > 0) result.append(", ", 2);
                String param = type_to_string(type->func_info.param_types[i]);
                result.append(param.data(), param.size());
            }
            result.append("): ", 3);
            String ret = type_to_string(type->func_info.return_type);
            result.append(ret.data(), ret.size());
            return result;
        }

        case TypeKind::TypeParam: {
            String result(type->type_param_info.name.data(), type->type_param_info.name.size());
            return result;
        }

        case TypeKind::ExceptionRef:
            return String("ExceptionRef");

        case TypeKind::Self:
            return String("Self");

        default:
            return String("unknown");
    }
}

// --- Local variable collection ---

// Check if an AstKind is a statement (not a declaration)
static bool is_stmt_kind(AstKind kind) {
    return kind == AstKind::StmtExpr || kind == AstKind::StmtBlock ||
           kind == AstKind::StmtIf || kind == AstKind::StmtWhile ||
           kind == AstKind::StmtFor || kind == AstKind::StmtReturn ||
           kind == AstKind::StmtTry;
}

// Walk an AST statement recursively, collecting variable declarations
static void collect_vars_from_stmt(Stmt* stmt, tsl::robin_map<String, Type*>& out) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtBlock:
            for (auto* decl : stmt->block.declarations) {
                if (!decl) continue;
                if (decl->kind == AstKind::DeclVar) {
                    if (!decl->var_decl.name.empty() && decl->var_decl.resolved_type) {
                        out[String(decl->var_decl.name)] = decl->var_decl.resolved_type;
                    }
                } else if (is_stmt_kind(decl->kind)) {
                    collect_vars_from_stmt(&decl->stmt, out);
                }
            }
            break;
        case AstKind::StmtIf:
            collect_vars_from_stmt(stmt->if_stmt.then_branch, out);
            collect_vars_from_stmt(stmt->if_stmt.else_branch, out);
            break;
        case AstKind::StmtWhile:
            collect_vars_from_stmt(stmt->while_stmt.body, out);
            break;
        case AstKind::StmtFor:
            if (stmt->for_stmt.initializer && stmt->for_stmt.initializer->kind == AstKind::DeclVar) {
                Decl* init = stmt->for_stmt.initializer;
                if (!init->var_decl.name.empty() && init->var_decl.resolved_type) {
                    out[String(init->var_decl.name)] = init->var_decl.resolved_type;
                }
            }
            collect_vars_from_stmt(stmt->for_stmt.body, out);
            break;
        case AstKind::StmtTry:
            collect_vars_from_stmt(stmt->try_stmt.try_body, out);
            for (u32 i = 0; i < stmt->try_stmt.catches.size(); i++) {
                const CatchClause& cc = stmt->try_stmt.catches[i];
                if (!cc.var_name.empty() && cc.resolved_type) {
                    out[String(cc.var_name)] = cc.resolved_type;
                }
                collect_vars_from_stmt(cc.body, out);
            }
            collect_vars_from_stmt(stmt->try_stmt.finally_body, out);
            break;
        default:
            break;
    }
}

// Collect parameters from a function-like decl
static void collect_params(Span<Param> params, tsl::robin_map<String, Type*>& out) {
    for (u32 i = 0; i < params.size(); i++) {
        if (!params[i].name.empty() && params[i].resolved_type) {
            out[String(params[i].name)] = params[i].resolved_type;
        }
    }
}

void LspAnalysisContext::collect_local_variables(
    Decl* decl, tsl::robin_map<String, Type*>& out)
{
    if (!decl) return;

    switch (decl->kind) {
        case AstKind::DeclFun: {
            collect_params(decl->fun_decl.params, out);
            collect_vars_from_stmt(decl->fun_decl.body, out);
            break;
        }
        case AstKind::DeclMethod: {
            // Add "self" with the struct's type
            Type* self_type = m_type_env->named_type_by_name(decl->method_decl.struct_name);
            if (self_type) {
                out[String("self")] = self_type;
            }
            collect_params(decl->method_decl.params, out);
            collect_vars_from_stmt(decl->method_decl.body, out);
            break;
        }
        case AstKind::DeclConstructor: {
            Type* self_type = m_type_env->named_type_by_name(decl->constructor_decl.struct_name);
            if (self_type) {
                out[String("self")] = self_type;
            }
            collect_params(decl->constructor_decl.params, out);
            collect_vars_from_stmt(decl->constructor_decl.body, out);
            break;
        }
        case AstKind::DeclDestructor: {
            Type* self_type = m_type_env->named_type_by_name(decl->destructor_decl.struct_name);
            if (self_type) {
                out[String("self")] = self_type;
            }
            collect_params(decl->destructor_decl.params, out);
            collect_vars_from_stmt(decl->destructor_decl.body, out);
            break;
        }
        default:
            break;
    }
}

// --- Local variable name collection (lightweight, no types) ---

static void collect_names_from_stmt(Stmt* stmt, tsl::robin_set<String>& out) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtBlock:
            for (auto* decl : stmt->block.declarations) {
                if (!decl) continue;
                if (decl->kind == AstKind::DeclVar && !decl->var_decl.name.empty()) {
                    out.insert(String(decl->var_decl.name));
                } else if (is_stmt_kind(decl->kind)) {
                    collect_names_from_stmt(&decl->stmt, out);
                }
            }
            break;
        case AstKind::StmtIf:
            collect_names_from_stmt(stmt->if_stmt.then_branch, out);
            collect_names_from_stmt(stmt->if_stmt.else_branch, out);
            break;
        case AstKind::StmtWhile:
            collect_names_from_stmt(stmt->while_stmt.body, out);
            break;
        case AstKind::StmtFor:
            if (stmt->for_stmt.initializer && stmt->for_stmt.initializer->kind == AstKind::DeclVar) {
                if (!stmt->for_stmt.initializer->var_decl.name.empty()) {
                    out.insert(String(stmt->for_stmt.initializer->var_decl.name));
                }
            }
            collect_names_from_stmt(stmt->for_stmt.body, out);
            break;
        case AstKind::StmtTry:
            collect_names_from_stmt(stmt->try_stmt.try_body, out);
            for (u32 i = 0; i < stmt->try_stmt.catches.size(); i++) {
                if (!stmt->try_stmt.catches[i].var_name.empty()) {
                    out.insert(String(stmt->try_stmt.catches[i].var_name));
                }
                collect_names_from_stmt(stmt->try_stmt.catches[i].body, out);
            }
            collect_names_from_stmt(stmt->try_stmt.finally_body, out);
            break;
        default:
            break;
    }
}

static void collect_param_names(Span<Param> params, tsl::robin_set<String>& out) {
    for (u32 i = 0; i < params.size(); i++) {
        if (!params[i].name.empty()) {
            out.insert(String(params[i].name));
        }
    }
}

void LspAnalysisContext::collect_local_var_names(
    Decl* decl, tsl::robin_set<String>& out)
{
    if (!decl) return;

    switch (decl->kind) {
        case AstKind::DeclFun:
            collect_param_names(decl->fun_decl.params, out);
            collect_names_from_stmt(decl->fun_decl.body, out);
            break;
        case AstKind::DeclMethod:
            out.insert(String("self"));
            collect_param_names(decl->method_decl.params, out);
            collect_names_from_stmt(decl->method_decl.body, out);
            break;
        case AstKind::DeclConstructor:
            out.insert(String("self"));
            collect_param_names(decl->constructor_decl.params, out);
            collect_names_from_stmt(decl->constructor_decl.body, out);
            break;
        case AstKind::DeclDestructor:
            out.insert(String("self"));
            collect_param_names(decl->destructor_decl.params, out);
            collect_names_from_stmt(decl->destructor_decl.body, out);
            break;
        default:
            break;
    }
}

// --- CST expression type resolution ---

// Helper: look up a field type from a struct type, walking the inheritance chain
static Type* find_field_type_in_hierarchy(Type* struct_type, StringView field_name) {
    Type* current = struct_type;
    u32 depth = 0;
    while (current && current->kind == TypeKind::Struct && depth < 16) {
        const FieldInfo* field = current->struct_info.find_field(field_name);
        if (field) return field->type;
        current = current->struct_info.parent;
        depth++;
    }
    return nullptr;
}

Type* LspAnalysisContext::resolve_cst_expr_type(
    SyntaxNode* expr_node,
    const tsl::robin_map<String, Type*>& local_vars)
{
    if (!expr_node) return nullptr;

    switch (expr_node->kind) {
        case SyntaxKind::NodeIdentifierExpr: {
            for (u32 i = 0; i < expr_node->children.size(); i++) {
                if (expr_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    auto it = local_vars.find(String(expr_node->children[i]->token.text()));
                    if (it != local_vars.end()) return it->second;
                    break;
                }
            }
            return nullptr;
        }
        case SyntaxKind::TokenIdentifier: {
            auto it = local_vars.find(String(expr_node->token.text()));
            if (it != local_vars.end()) return it->second;
            return nullptr;
        }
        case SyntaxKind::NodeSelfExpr:
        case SyntaxKind::TokenKwSelf: {
            auto it = local_vars.find(String("self"));
            if (it != local_vars.end()) return it->second;
            return nullptr;
        }
        case SyntaxKind::NodeGetExpr: {
            if (expr_node->children.size() >= 3) {
                Type* receiver = resolve_cst_expr_type(expr_node->children[0], local_vars);
                if (!receiver) return nullptr;

                // Unwrap reference types to get the inner type
                if (receiver->is_reference()) {
                    receiver = receiver->ref_info.inner_type;
                }

                SyntaxNode* member_node = expr_node->children[expr_node->children.size() - 1];
                if (member_node->kind == SyntaxKind::TokenIdentifier) {
                    StringView field_name = member_node->token.text();

                    // Check fields first
                    if (receiver->kind == TypeKind::Struct) {
                        Type* field_type = find_field_type_in_hierarchy(receiver, field_name);
                        if (field_type) return field_type;
                    }

                    // Check methods
                    Type* found_in = nullptr;
                    const MethodInfo* method = m_type_env->types().lookup_method(
                        receiver, field_name, &found_in);
                    if (method) return method->return_type;
                }
            }
            return nullptr;
        }
        case SyntaxKind::NodeCallExpr: {
            if (expr_node->children.size() > 0) {
                SyntaxNode* callee = expr_node->children[0];
                if (callee->kind == SyntaxKind::NodeIdentifierExpr) {
                    // Function call or constructor call
                    for (u32 i = 0; i < callee->children.size(); i++) {
                        if (callee->children[i]->kind == SyntaxKind::TokenIdentifier) {
                            StringView name = callee->children[i]->token.text();
                            // Check if it's a function in the declaration symbols
                            if (m_decl_symbols) {
                                Symbol* sym = m_decl_symbols->lookup(name);
                                if (sym && sym->type) {
                                    if (sym->type->kind == TypeKind::Function) {
                                        return sym->type->func_info.return_type;
                                    }
                                    // Constructor call (struct name)
                                    if (sym->kind == SymbolKind::Struct) {
                                        return sym->type;
                                    }
                                }
                            }
                            break;
                        }
                    }
                } else if (callee->kind == SyntaxKind::NodeGetExpr) {
                    // Method call: resolve receiver, look up method return type
                    if (callee->children.size() >= 3) {
                        Type* receiver = resolve_cst_expr_type(callee->children[0], local_vars);
                        if (receiver) {
                            if (receiver->is_reference()) {
                                receiver = receiver->ref_info.inner_type;
                            }
                            SyntaxNode* method_node =
                                callee->children[callee->children.size() - 1];
                            if (method_node->kind == SyntaxKind::TokenIdentifier) {
                                Type* found_in = nullptr;
                                const MethodInfo* method = m_type_env->types().lookup_method(
                                    receiver, method_node->token.text(), &found_in);
                                if (method) return method->return_type;
                            }
                        }
                    }
                }
            }
            return nullptr;
        }
        case SyntaxKind::NodeStructLiteralExpr: {
            // Struct literal: first identifier is the type name
            for (u32 i = 0; i < expr_node->children.size(); i++) {
                if (expr_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    StringView name = expr_node->children[i]->token.text();
                    return m_type_env->named_type_by_name(name);
                }
            }
            return nullptr;
        }
        default:
            return nullptr;
    }
}

} // namespace rx

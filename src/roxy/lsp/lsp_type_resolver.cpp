#include "roxy/lsp/lsp_type_resolver.hpp"

namespace rx {

LspTypeResolver::LspTypeResolver(const GlobalIndex& index)
    : m_index(index) {}

void LspTypeResolver::analyze_function(Decl* fun_decl) {
    if (!fun_decl) return;

    m_var_types.clear();
    m_self_type.clear();

    if (fun_decl->kind == AstKind::DeclFun) {
        // Register parameters
        for (u32 i = 0; i < fun_decl->fun_decl.params.size(); i++) {
            const Param& param = fun_decl->fun_decl.params[i];
            if (!param.name.empty() && param.type && !param.type->name.empty()) {
                m_var_types[String(param.name)] = String(param.type->name);
            }
        }
        // Walk body
        if (fun_decl->fun_decl.body) {
            analyze_block(fun_decl->fun_decl.body);
        }
    } else if (fun_decl->kind == AstKind::DeclMethod) {
        m_self_type = String(fun_decl->method_decl.struct_name);

        // Register parameters
        for (u32 i = 0; i < fun_decl->method_decl.params.size(); i++) {
            const Param& param = fun_decl->method_decl.params[i];
            if (!param.name.empty() && param.type && !param.type->name.empty()) {
                m_var_types[String(param.name)] = String(param.type->name);
            }
        }
        // Walk body
        if (fun_decl->method_decl.body) {
            analyze_block(fun_decl->method_decl.body);
        }
    } else if (fun_decl->kind == AstKind::DeclConstructor) {
        m_self_type = String(fun_decl->constructor_decl.struct_name);

        for (u32 i = 0; i < fun_decl->constructor_decl.params.size(); i++) {
            const Param& param = fun_decl->constructor_decl.params[i];
            if (!param.name.empty() && param.type && !param.type->name.empty()) {
                m_var_types[String(param.name)] = String(param.type->name);
            }
        }
        if (fun_decl->constructor_decl.body) {
            analyze_block(fun_decl->constructor_decl.body);
        }
    } else if (fun_decl->kind == AstKind::DeclDestructor) {
        m_self_type = String(fun_decl->destructor_decl.struct_name);

        for (u32 i = 0; i < fun_decl->destructor_decl.params.size(); i++) {
            const Param& param = fun_decl->destructor_decl.params[i];
            if (!param.name.empty() && param.type && !param.type->name.empty()) {
                m_var_types[String(param.name)] = String(param.type->name);
            }
        }
        if (fun_decl->destructor_decl.body) {
            analyze_block(fun_decl->destructor_decl.body);
        }
    }
}

void LspTypeResolver::analyze_block(Stmt* block) {
    if (!block || block->kind != AstKind::StmtBlock) return;

    for (u32 i = 0; i < block->block.declarations.size(); i++) {
        analyze_decl(block->block.declarations[i]);
    }
}

void LspTypeResolver::analyze_stmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtBlock:
            analyze_block(stmt);
            break;
        case AstKind::StmtIf:
            if (stmt->if_stmt.then_branch) analyze_stmt(stmt->if_stmt.then_branch);
            if (stmt->if_stmt.else_branch) analyze_stmt(stmt->if_stmt.else_branch);
            break;
        case AstKind::StmtWhile:
            if (stmt->while_stmt.body) analyze_stmt(stmt->while_stmt.body);
            break;
        case AstKind::StmtFor:
            if (stmt->for_stmt.initializer) analyze_decl(stmt->for_stmt.initializer);
            if (stmt->for_stmt.body) analyze_stmt(stmt->for_stmt.body);
            break;
        case AstKind::StmtTry:
            if (stmt->try_stmt.try_body) analyze_stmt(stmt->try_stmt.try_body);
            for (u32 i = 0; i < stmt->try_stmt.catches.size(); i++) {
                if (stmt->try_stmt.catches[i].body) {
                    analyze_stmt(stmt->try_stmt.catches[i].body);
                }
            }
            if (stmt->try_stmt.finally_body) analyze_stmt(stmt->try_stmt.finally_body);
            break;
        default:
            break;
    }
}

void LspTypeResolver::analyze_decl(Decl* decl) {
    if (!decl) return;

    if (decl->kind == AstKind::DeclVar) {
        String type_name;

        // Try explicit type annotation first
        if (decl->var_decl.type && !decl->var_decl.type->name.empty()) {
            type_name = String(decl->var_decl.type->name);
        }
        // Otherwise, infer from initializer
        else if (decl->var_decl.initializer) {
            type_name = resolve_ast_expr_type(decl->var_decl.initializer);
        }

        // Always register variable in scope (even with empty type) to prevent
        // cascade diagnostics — "unresolved identifier" for declared variables
        if (!decl->var_decl.name.empty()) {
            m_var_types[String(decl->var_decl.name)] = std::move(type_name);
        }
    }

    // Walk into nested statements (if, while, for, etc.)
    if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtYield) {
        analyze_stmt(&decl->stmt);
    }
}

String LspTypeResolver::resolve_ast_expr_type(Expr* expr) const {
    if (!expr) return String();

    switch (expr->kind) {
        case AstKind::ExprIdentifier: {
            // Look up in scope
            auto it = m_var_types.find(String(expr->identifier.name));
            if (it != m_var_types.end()) {
                return it->second;
            }
            return String();
        }
        case AstKind::ExprThis: {
            return m_self_type;
        }
        case AstKind::ExprStructLiteral: {
            // Type name comes from the struct literal
            if (!expr->struct_literal.type_name.empty()) {
                return String(expr->struct_literal.type_name);
            }
            return String();
        }
        case AstKind::ExprCall: {
            // Try to resolve the callee's return type
            if (expr->call.callee) {
                if (expr->call.callee->kind == AstKind::ExprIdentifier) {
                    StringView callee_name = expr->call.callee->identifier.name;
                    // Check if it's a function call
                    StringView return_type = m_index.find_function_return_type(callee_name);
                    if (!return_type.empty()) {
                        return String(return_type);
                    }
                    // Could be a constructor call — return the type name
                    if (m_index.find_struct(callee_name)) {
                        return String(callee_name);
                    }
                } else if (expr->call.callee->kind == AstKind::ExprGet) {
                    // Method call: obj.method()
                    String receiver_type = resolve_ast_expr_type(expr->call.callee->get.object);
                    if (!receiver_type.empty()) {
                        StringView return_type = m_index.find_method_return_type(
                            StringView(receiver_type.data(), receiver_type.size()),
                            expr->call.callee->get.name);
                        if (!return_type.empty()) {
                            return String(return_type);
                        }
                    }
                }
            }
            return String();
        }
        case AstKind::ExprGet: {
            // Resolve field access: obj.field
            String receiver_type = resolve_ast_expr_type(expr->get.object);
            if (!receiver_type.empty()) {
                return resolve_field_type_in_hierarchy(
                    StringView(receiver_type.data(), receiver_type.size()),
                    expr->get.name);
            }
            return String();
        }
        default:
            return String();
    }
}

String LspTypeResolver::resolve_cst_expr_type(SyntaxNode* expr_node) const {
    if (!expr_node) return String();

    switch (expr_node->kind) {
        case SyntaxKind::NodeIdentifierExpr: {
            SyntaxNode* ident = nullptr;
            for (u32 i = 0; i < expr_node->children.size(); i++) {
                if (expr_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    ident = expr_node->children[i];
                    break;
                }
            }
            if (ident) {
                auto it = m_var_types.find(String(ident->token.text()));
                if (it != m_var_types.end()) {
                    return it->second;
                }
            }
            return String();
        }
        case SyntaxKind::TokenIdentifier: {
            // Bare identifier token (not wrapped in NodeIdentifierExpr)
            auto it = m_var_types.find(String(expr_node->token.text()));
            if (it != m_var_types.end()) {
                return it->second;
            }
            return String();
        }
        case SyntaxKind::NodeSelfExpr:
        case SyntaxKind::TokenKwSelf: {
            return m_self_type;
        }
        case SyntaxKind::NodeGetExpr: {
            // Chained access: resolve object type, then look up field
            if (expr_node->children.size() >= 3) {
                String receiver_type = resolve_cst_expr_type(expr_node->children[0]);
                if (!receiver_type.empty()) {
                    SyntaxNode* member_node = expr_node->children[expr_node->children.size() - 1];
                    if (member_node->kind == SyntaxKind::TokenIdentifier) {
                        return resolve_field_type_in_hierarchy(
                            StringView(receiver_type.data(), receiver_type.size()),
                            member_node->token.text());
                    }
                }
            }
            return String();
        }
        case SyntaxKind::NodeCallExpr: {
            // Function/method call: resolve return type
            if (expr_node->children.size() > 0) {
                SyntaxNode* callee = expr_node->children[0];
                if (callee->kind == SyntaxKind::NodeIdentifierExpr) {
                    SyntaxNode* ident = nullptr;
                    for (u32 i = 0; i < callee->children.size(); i++) {
                        if (callee->children[i]->kind == SyntaxKind::TokenIdentifier) {
                            ident = callee->children[i];
                            break;
                        }
                    }
                    if (ident) {
                        StringView callee_name = ident->token.text();
                        StringView return_type = m_index.find_function_return_type(callee_name);
                        if (!return_type.empty()) return String(return_type);
                        if (m_index.find_struct(callee_name)) return String(callee_name);
                    }
                } else if (callee->kind == SyntaxKind::NodeGetExpr) {
                    // Method call: obj.method()
                    if (callee->children.size() >= 3) {
                        String receiver_type = resolve_cst_expr_type(callee->children[0]);
                        if (!receiver_type.empty()) {
                            SyntaxNode* method_name_node = callee->children[callee->children.size() - 1];
                            if (method_name_node->kind == SyntaxKind::TokenIdentifier) {
                                StringView return_type = m_index.find_method_return_type(
                                    StringView(receiver_type.data(), receiver_type.size()),
                                    method_name_node->token.text());
                                if (!return_type.empty()) return String(return_type);
                            }
                        }
                    }
                }
            }
            return String();
        }
        case SyntaxKind::NodeStructLiteralExpr: {
            // Struct literal: first identifier is the type name
            for (u32 i = 0; i < expr_node->children.size(); i++) {
                if (expr_node->children[i]->kind == SyntaxKind::TokenIdentifier) {
                    return String(expr_node->children[i]->token.text());
                }
            }
            return String();
        }
        default:
            return String();
    }
}

String LspTypeResolver::resolve_field_type_in_hierarchy(
    StringView struct_name, StringView field_name) const {

    StringView current = struct_name;
    u32 depth = 0;
    while (!current.empty() && depth < 16) {
        // Check field type
        StringView field_type = m_index.find_field_type(current, field_name);
        if (!field_type.empty()) return String(field_type);

        // Check method return type (for method calls)
        StringView method_return = m_index.find_method_return_type(current, field_name);
        if (!method_return.empty()) return String(method_return);

        // Walk to parent
        current = m_index.find_struct_parent(current);
        depth++;
    }

    return String();
}

// --- Diagnostic collection ---

void LspTypeResolver::analyze_function_with_diagnostics(Decl* fun_decl) {
    m_diagnostics.clear();

    // First run normal analysis to populate variable scope
    analyze_function(fun_decl);

    // Then walk the AST to check for semantic issues
    if (!fun_decl) return;

    Stmt* body = nullptr;
    if (fun_decl->kind == AstKind::DeclFun) {
        body = fun_decl->fun_decl.body;
    } else if (fun_decl->kind == AstKind::DeclMethod) {
        body = fun_decl->method_decl.body;
    } else if (fun_decl->kind == AstKind::DeclConstructor) {
        body = fun_decl->constructor_decl.body;
    } else if (fun_decl->kind == AstKind::DeclDestructor) {
        body = fun_decl->destructor_decl.body;
    }

    if (body) {
        check_stmt(body);
    }
}

void LspTypeResolver::add_diagnostic(TextRange range, DiagnosticSeverity severity, String message) {
    SemanticDiagnostic diag;
    diag.range = range;
    diag.severity = severity;
    diag.message = std::move(message);
    m_diagnostics.push_back(std::move(diag));
}

bool LspTypeResolver::is_known_type(StringView name) const {
    // Primitives
    if (name == "i8" || name == "i16" || name == "i32" || name == "i64" ||
        name == "u8" || name == "u16" || name == "u32" || name == "u64" ||
        name == "f32" || name == "f64" || name == "bool" || name == "string" || name == "void") {
        return true;
    }
    // Builtins
    if (name == "List" || name == "Map" || name == "Coro" || name == "ExceptionRef") {
        return true;
    }
    // GlobalIndex-known types
    if (m_index.find_struct(name) || m_index.find_enum(name) || m_index.find_trait(name)) {
        return true;
    }
    return false;
}

bool LspTypeResolver::has_field_in_hierarchy(StringView struct_name, StringView field_name) const {
    StringView current = struct_name;
    u32 depth = 0;
    while (!current.empty() && depth < 16) {
        if (m_index.find_field(current, field_name)) return true;
        current = m_index.find_struct_parent(current);
        depth++;
    }
    return false;
}

bool LspTypeResolver::has_method_in_hierarchy(StringView struct_name, StringView method_name) const {
    StringView current = struct_name;
    u32 depth = 0;
    while (!current.empty() && depth < 16) {
        if (m_index.find_method(current, method_name)) return true;
        current = m_index.find_struct_parent(current);
        depth++;
    }
    return false;
}

void LspTypeResolver::check_type_annotation(TypeExpr* type_expr) {
    if (!type_expr || type_expr->name.empty()) return;

    if (!is_known_type(type_expr->name)) {
        TextRange range{type_expr->loc.offset, type_expr->loc.end_offset};
        // Use name length as fallback if end_offset is 0
        if (range.end <= range.start) {
            range.end = range.start + static_cast<u32>(type_expr->name.size());
        }
        String msg("Unknown type '");
        msg.append(type_expr->name);
        msg.push_back('\'');
        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
    }

    // Check type args recursively
    for (u32 i = 0; i < type_expr->type_args.size(); i++) {
        check_type_annotation(type_expr->type_args[i]);
    }
}

void LspTypeResolver::check_decl(Decl* decl) {
    if (!decl) return;

    if (decl->kind == AstKind::DeclVar) {
        // Check type annotation
        if (decl->var_decl.type) {
            check_type_annotation(decl->var_decl.type);
        }
        // Check initializer
        if (decl->var_decl.initializer) {
            check_expr(decl->var_decl.initializer);
        }
    }

    // Walk into nested statements
    if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtYield) {
        check_stmt(&decl->stmt);
    }
}

void LspTypeResolver::check_stmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtBlock:
            for (u32 i = 0; i < stmt->block.declarations.size(); i++) {
                check_decl(stmt->block.declarations[i]);
            }
            break;
        case AstKind::StmtExpr:
            if (stmt->expr_stmt.expr) check_expr(stmt->expr_stmt.expr);
            break;
        case AstKind::StmtIf:
            if (stmt->if_stmt.condition) check_expr(stmt->if_stmt.condition);
            if (stmt->if_stmt.then_branch) check_stmt(stmt->if_stmt.then_branch);
            if (stmt->if_stmt.else_branch) check_stmt(stmt->if_stmt.else_branch);
            break;
        case AstKind::StmtWhile:
            if (stmt->while_stmt.condition) check_expr(stmt->while_stmt.condition);
            if (stmt->while_stmt.body) check_stmt(stmt->while_stmt.body);
            break;
        case AstKind::StmtFor:
            if (stmt->for_stmt.initializer) check_decl(stmt->for_stmt.initializer);
            if (stmt->for_stmt.condition) check_expr(stmt->for_stmt.condition);
            if (stmt->for_stmt.increment) check_expr(stmt->for_stmt.increment);
            if (stmt->for_stmt.body) check_stmt(stmt->for_stmt.body);
            break;
        case AstKind::StmtReturn:
            if (stmt->return_stmt.value) check_expr(stmt->return_stmt.value);
            break;
        case AstKind::StmtThrow:
            if (stmt->throw_stmt.expr) check_expr(stmt->throw_stmt.expr);
            break;
        case AstKind::StmtTry:
            if (stmt->try_stmt.try_body) check_stmt(stmt->try_stmt.try_body);
            for (u32 i = 0; i < stmt->try_stmt.catches.size(); i++) {
                if (stmt->try_stmt.catches[i].body) {
                    check_stmt(stmt->try_stmt.catches[i].body);
                }
            }
            if (stmt->try_stmt.finally_body) check_stmt(stmt->try_stmt.finally_body);
            break;
        case AstKind::StmtDelete:
            if (stmt->delete_stmt.expr) check_expr(stmt->delete_stmt.expr);
            break;
        case AstKind::StmtYield:
            if (stmt->yield_stmt.value) check_expr(stmt->yield_stmt.value);
            break;
        default:
            break;
    }
}

void LspTypeResolver::check_expr(Expr* expr) {
    if (!expr) return;

    switch (expr->kind) {
        case AstKind::ExprIdentifier: {
            StringView name = expr->identifier.name;
            if (name.empty()) break;

            // Check if known: local var, global symbol, or type name
            auto var_it = m_var_types.find(String(name));
            if (var_it != m_var_types.end()) break; // known local

            if (m_index.find_function(name) || m_index.find_struct(name) ||
                m_index.find_enum(name) || m_index.find_trait(name) ||
                m_index.find_global(name)) {
                break; // known global
            }

            // Also accept: true, false, nil are literals (not identifiers)
            // But just in case the lowering creates them as identifiers:
            if (name == "true" || name == "false" || name == "nil") break;

            TextRange range{expr->loc.offset, expr->loc.end_offset};
            if (range.end <= range.start) {
                range.end = range.start + static_cast<u32>(name.size());
            }
            String msg("Unresolved identifier '");
            msg.append(name);
            msg.push_back('\'');
            add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
            break;
        }
        case AstKind::ExprCall: {
            Expr* callee = expr->call.callee;
            if (!callee) break;

            if (callee->kind == AstKind::ExprIdentifier) {
                StringView callee_name = callee->identifier.name;

                // Check if function/constructor exists
                bool callee_found = false;
                if (m_index.find_function(callee_name)) {
                    callee_found = true;

                    // Check argument count
                    i32 expected = m_index.find_function_param_count(callee_name);
                    i32 actual = static_cast<i32>(expr->call.arguments.size());
                    if (expected >= 0 && actual != expected) {
                        TextRange range{expr->loc.offset, expr->loc.end_offset};
                        if (range.end <= range.start) range.end = range.start + 1;
                        String msg("Expected ");
                        msg.append(std::to_string(expected).c_str());
                        msg.append(" arguments, got ");
                        msg.append(std::to_string(actual).c_str());
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    }
                } else if (m_index.find_struct(callee_name)) {
                    callee_found = true;
                    // Constructor call — check constructor param count
                    // Default constructor name is "new"
                    i32 expected = m_index.find_constructor_param_count(callee_name, "new");
                    i32 actual = static_cast<i32>(expr->call.arguments.size());
                    if (expected >= 0 && actual != expected) {
                        TextRange range{expr->loc.offset, expr->loc.end_offset};
                        if (range.end <= range.start) range.end = range.start + 1;
                        String msg("Expected ");
                        msg.append(std::to_string(expected).c_str());
                        msg.append(" arguments, got ");
                        msg.append(std::to_string(actual).c_str());
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    }
                }

                if (!callee_found) {
                    // Also check if it's a local variable (callable)
                    auto var_it = m_var_types.find(String(callee_name));
                    if (var_it == m_var_types.end()) {
                        TextRange range{callee->loc.offset, callee->loc.end_offset};
                        if (range.end <= range.start) {
                            range.end = range.start + static_cast<u32>(callee_name.size());
                        }
                        String msg("Unresolved function '");
                        msg.append(callee_name);
                        msg.push_back('\'');
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    }
                }
            } else if (callee->kind == AstKind::ExprGet) {
                // Method call: obj.method()
                String receiver_type = resolve_ast_expr_type(callee->get.object);
                if (!receiver_type.empty()) {
                    StringView method_name = callee->get.name;
                    StringView recv_sv(receiver_type.data(), receiver_type.size());

                    // Skip method checks when receiver type is unknown (cascade prevention)
                    if (!m_index.find_struct(recv_sv) && !m_index.find_trait(recv_sv)) {
                        // Unknown receiver type — skip
                    } else if (!has_method_in_hierarchy(recv_sv, method_name)) {
                        TextRange range{callee->loc.offset, callee->loc.end_offset};
                        if (range.end <= range.start) {
                            range.end = range.start + static_cast<u32>(method_name.size());
                        }
                        String msg("No method '");
                        msg.append(method_name);
                        msg.append("' on type '");
                        msg.append(recv_sv);
                        msg.push_back('\'');
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    } else {
                        // Method found — check arg count
                        i32 expected = m_index.find_method_param_count(recv_sv, method_name);
                        // Walk hierarchy if not found at this level
                        StringView current = recv_sv;
                        u32 depth = 0;
                        while (expected < 0 && !current.empty() && depth < 16) {
                            current = m_index.find_struct_parent(current);
                            if (!current.empty()) {
                                expected = m_index.find_method_param_count(current, method_name);
                            }
                            depth++;
                        }
                        i32 actual = static_cast<i32>(expr->call.arguments.size());
                        if (expected >= 0 && actual != expected) {
                            TextRange range{expr->loc.offset, expr->loc.end_offset};
                            if (range.end <= range.start) range.end = range.start + 1;
                            String msg("Expected ");
                            msg.append(std::to_string(expected).c_str());
                            msg.append(" arguments, got ");
                            msg.append(std::to_string(actual).c_str());
                            add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                        }
                    }
                }
                // Recurse into receiver
                check_expr(callee->get.object);
            } else {
                // Other callee types: just recurse
                check_expr(callee);
            }

            // Recurse into arguments
            for (u32 i = 0; i < expr->call.arguments.size(); i++) {
                check_expr(expr->call.arguments[i].expr);
            }
            break;
        }
        case AstKind::ExprGet: {
            // Field access: obj.field
            String receiver_type = resolve_ast_expr_type(expr->get.object);
            if (!receiver_type.empty()) {
                StringView recv_sv(receiver_type.data(), receiver_type.size());

                // Skip field/method checks when receiver type is unknown (cascade prevention)
                if (m_index.find_struct(recv_sv) || m_index.find_trait(recv_sv)) {
                    StringView field_name = expr->get.name;

                    // Check field OR method exists
                    if (!has_field_in_hierarchy(recv_sv, field_name) &&
                        !has_method_in_hierarchy(recv_sv, field_name)) {
                        TextRange range{expr->loc.offset, expr->loc.end_offset};
                        if (range.end <= range.start) {
                            range.end = range.start + static_cast<u32>(field_name.size());
                        }
                        String msg("No field '");
                        msg.append(field_name);
                        msg.append("' on type '");
                        msg.append(recv_sv);
                        msg.push_back('\'');
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    }
                }
            }
            // Recurse into object
            check_expr(expr->get.object);
            break;
        }
        case AstKind::ExprStaticGet: {
            StringView type_name = expr->static_get.type_name;
            StringView member_name = expr->static_get.member_name;

            if (!type_name.empty() && !member_name.empty()) {
                if (m_index.find_enum(type_name)) {
                    // Check enum variant exists
                    const Vector<String>* variants = m_index.get_enum_variants(type_name);
                    bool found = false;
                    if (variants) {
                        for (u32 i = 0; i < variants->size(); i++) {
                            if (StringView((*variants)[i].data(), (*variants)[i].size()) == member_name) {
                                found = true;
                                break;
                            }
                        }
                    }
                    if (!found) {
                        TextRange range{expr->loc.offset, expr->loc.end_offset};
                        if (range.end <= range.start) {
                            range.end = range.start + static_cast<u32>(type_name.size()) + 2 + static_cast<u32>(member_name.size());
                        }
                        String msg("No variant '");
                        msg.append(member_name);
                        msg.append("' on enum '");
                        msg.append(type_name);
                        msg.push_back('\'');
                        add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                    }
                } else if (!m_index.find_struct(type_name) && !m_index.find_trait(type_name)) {
                    TextRange range{expr->loc.offset, expr->loc.end_offset};
                    if (range.end <= range.start) {
                        range.end = range.start + static_cast<u32>(type_name.size());
                    }
                    String msg("Unknown type '");
                    msg.append(type_name);
                    msg.push_back('\'');
                    add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                }
            }
            break;
        }
        case AstKind::ExprStructLiteral: {
            StringView type_name = expr->struct_literal.type_name;
            if (!type_name.empty()) {
                if (!m_index.find_struct(type_name)) {
                    TextRange range{expr->loc.offset, expr->loc.end_offset};
                    if (range.end <= range.start) {
                        range.end = range.start + static_cast<u32>(type_name.size());
                    }
                    String msg("Unknown type '");
                    msg.append(type_name);
                    msg.push_back('\'');
                    add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                } else {
                    // Check each field name
                    for (u32 i = 0; i < expr->struct_literal.fields.size(); i++) {
                        StringView field_name = expr->struct_literal.fields[i].name;
                        if (!field_name.empty() && !has_field_in_hierarchy(type_name, field_name)) {
                            TextRange range{expr->struct_literal.fields[i].loc.offset,
                                          expr->struct_literal.fields[i].loc.end_offset};
                            if (range.end <= range.start) {
                                range.end = range.start + static_cast<u32>(field_name.size());
                            }
                            String msg("No field '");
                            msg.append(field_name);
                            msg.append("' on type '");
                            msg.append(type_name);
                            msg.push_back('\'');
                            add_diagnostic(range, DiagnosticSeverity::Error, std::move(msg));
                        }
                    }
                }
            }
            // Recurse into field initializer expressions
            for (u32 i = 0; i < expr->struct_literal.fields.size(); i++) {
                check_expr(expr->struct_literal.fields[i].value);
            }
            break;
        }
        case AstKind::ExprUnary:
            check_expr(expr->unary.operand);
            break;
        case AstKind::ExprBinary:
            check_expr(expr->binary.left);
            check_expr(expr->binary.right);
            break;
        case AstKind::ExprTernary:
            check_expr(expr->ternary.condition);
            check_expr(expr->ternary.then_expr);
            check_expr(expr->ternary.else_expr);
            break;
        case AstKind::ExprIndex:
            check_expr(expr->index.object);
            check_expr(expr->index.index);
            break;
        case AstKind::ExprAssign:
            check_expr(expr->assign.target);
            check_expr(expr->assign.value);
            break;
        case AstKind::ExprGrouping:
            check_expr(expr->grouping.expr);
            break;
        case AstKind::ExprStringInterp:
            for (u32 i = 0; i < expr->string_interp.expressions.size(); i++) {
                check_expr(expr->string_interp.expressions[i]);
            }
            break;
        default:
            break;
    }
}

} // namespace rx

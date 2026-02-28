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

        if (!type_name.empty() && !decl->var_decl.name.empty()) {
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

} // namespace rx

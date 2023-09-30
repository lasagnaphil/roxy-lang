#include "roxy/ast_printer.hpp"

#include "roxy/core/vector.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/type.hpp"

namespace rx {

std::string AstPrinter::to_string(const Expr* expr) {
    add(expr);
    std::string res = m_buf;
    reset();
    return res;
}

std::string AstPrinter::to_string(const Vector<Stmt*>& statements) {
    for (auto stmt : statements) {
        add(stmt);
        newline();
    }
    std::string res = m_buf;
    reset();
    return res;
}

void AstPrinter::add(const Expr* expr) {
    if (expr == nullptr) {
        m_buf += "null";
        return;
    }
    switch (expr->type) {
        case ExprType::Error: {
            m_buf += "error";
            break;
        }
        case ExprType::Assign: {
            auto& assign_expr = expr->cast<AssignExpr>();
            parenthesize(assign_expr.name.str(), assign_expr.value);
            break;
        }
        case ExprType::Binary: {
            auto& binary_expr = expr->cast<BinaryExpr>();
            parenthesize(binary_expr.op.str(), binary_expr.left, binary_expr.right);
            break;
        }
        case ExprType::Call: {
            auto& call_expr = expr->cast<CallExpr>();
            parenthesize("call", call_expr.callee, call_expr.arguments);
            break;
        }
        case ExprType::Ternary: {
            auto& ternary_expr = expr->cast<TernaryExpr>();
            parenthesize("ternary", ternary_expr.cond, ternary_expr.left, ternary_expr.right);
            break;
        }
        case ExprType::Grouping: {
            auto& grouping_expr = expr->cast<GroupingExpr>();
            parenthesize("group", grouping_expr.expression);
            break;
        }
        case ExprType::Literal: {
            auto& literal_expr = expr->cast<LiteralExpr>();
            m_buf += literal_expr.value.to_std_string();
            break;
        }
        case ExprType::Unary: {
            auto& unary_expr = expr->cast<UnaryExpr>();
            parenthesize(unary_expr.op.str(), unary_expr.right);
            break;
        }
        case ExprType::Variable: {
            auto& var_expr = expr->cast<VariableExpr>();
            m_buf += var_expr.name.str();
            break;
        }
    }
}

void AstPrinter::add(const Stmt* stmt) {
    if (stmt == nullptr) {
        m_buf += "null";
        return;
    }
    switch (stmt->type) {
        case StmtType::Error: {
            m_buf += "error";
            break;
        }
        case StmtType::Block: {
            auto& block_stmt = stmt->cast<BlockStmt>();
            parenthesize_block("block", block_stmt.statements);
            break;
        }
        case StmtType::Expression: {
            auto& expr_stmt = stmt->cast<ExpressionStmt>();
            parenthesize("expr", expr_stmt.expr);
            break;
        }
        case StmtType::Struct: {
            auto& struct_stmt = stmt->cast<StructStmt>();
            parenthesize("struct", struct_stmt.name.str(), struct_stmt.fields);
            break;
        }
        case StmtType::Function: {
            auto& fun_stmt = stmt->cast<FunctionStmt>();
            parenthesize("fun", fun_stmt.name.str(), fun_stmt.params, fun_stmt.body);
            break;
        }
        case StmtType::If: {
            auto& if_stmt = stmt->cast<IfStmt>();
            parenthesize("if", if_stmt.condition, if_stmt.then_branch, if_stmt.else_branch);
            break;
        }
        case StmtType::Print: {
            auto& print_stmt = stmt->cast<PrintStmt>();
            parenthesize("print", print_stmt.expr);
            break;
        }
        case StmtType::Var: {
            auto& var_stmt = stmt->cast<VarStmt>();
            parenthesize("var", var_stmt.var, var_stmt.initializer);
            break;
        }
        case StmtType::While: {
            auto& while_stmt = stmt->cast<WhileStmt>();
            parenthesize("while", while_stmt.condition, while_stmt.body);
            break;
        }
        case StmtType::Return: {
            auto& return_stmt = stmt->cast<ReturnStmt>();
            parenthesize("return", return_stmt.expr);
            break;
        }
        case StmtType::Break: {
            m_buf += "break";
            break;
        }
        case StmtType::Continue: {
            m_buf += "continue";
            break;
        }
    }
}

void AstPrinter::add(const Type* type) {
    if (type == nullptr) {
        m_buf += "null";
        return;
    }

    switch (type->kind) {
        case TypeKind::Primitive: {
            auto& prim_type = type->cast<PrimitiveType>();
            switch (prim_type.prim_kind) {
                case PrimTypeKind::Bool: m_buf += "bool"; break;
                case PrimTypeKind::Number: m_buf += "number"; break;
                case PrimTypeKind::String: m_buf += "string"; break;
            }
            break;
        }
        default: {
            // TODO
            m_buf += "unsupported"; break;
        }
    }
}

void AstPrinter::add(const Vector<Expr*>& expressions) {
    for (u32 i = 0; i < expressions.size() - 1; i++) {
        add(expressions[i]);
        m_buf += ' ';
    }
    add(expressions[expressions.size() - 1]);
}

void AstPrinter::add(const Vector<Stmt*>& statements) {
    inc_indent();
    for (u32 i = 0; i < statements.size(); i++) {
        newline();
        add(statements[i]);
    }
    dec_indent();
}

void AstPrinter::add(const Vector<Token>& tokens) {
    for (u32 i = 0; i < tokens.size() - 1; i++) {
        m_buf += tokens[i].str();
        m_buf += ' ';
    }
    m_buf += tokens[tokens.size() - 1].str();
}

void AstPrinter::add(const VarDecl& variable) {
    parenthesize("var", variable.name.str(), variable.type);
}

void AstPrinter::add(const Vector<VarDecl>& variables) {
    for (u32 i = 0; i < variables.size() - 1; i++) {
        add(variables[i]);
        m_buf += ' ';
    }
    add(variables[variables.size() - 1]);
}

void AstPrinter::parenthesize_block(std::string_view name, const Vector<Stmt*>& statements) {
    m_buf += "(";
    m_buf += name;
    inc_indent();
    newline();
    for (Stmt* stmt : statements) {
        add(stmt);
        if (stmt != statements.back()) newline();
    }
    dec_indent();
    m_buf += ")";
}

}
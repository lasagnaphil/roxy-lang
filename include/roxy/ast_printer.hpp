#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/ast_visitor.hpp"
#include "roxy/token.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/type.hpp"

#include <string>

namespace rx {

class AstPrinter :
        public ExprVisitorBase<AstPrinter, void>,
        public StmtVisitorBase<AstPrinter, void>,
        public TypeVisitorBase<AstPrinter, void> {

public:
    using ExprVisitorBase<AstPrinter, void>::visit;
    using StmtVisitorBase<AstPrinter, void>::visit;
    using TypeVisitorBase<AstPrinter, void>::visit;

    AstPrinter(const u8* source) : m_source(source) {}

    std::string_view get_token_str(Token token) {
        return token.str(m_source);
    }

    std::string to_string(Stmt& stmt) {
        visit(stmt);
        auto res = m_buf;
        m_buf.clear();
        return res;
    }

    void visit_impl(ErrorStmt& stmt) {
        add_identifier("error");
    }
    void visit_impl(BlockStmt& stmt) {
        begin_paren("block");
        inc_indent();
        for (auto& stmt : stmt.statements) {
            newline();
            visit(*stmt);
        }
        end_paren(); dec_indent();
    }
    void visit_impl(ExpressionStmt& stmt) {
        begin_paren("expr");
        visit(*stmt.expr);
        end_paren();
    }
    void visit_impl(StructStmt& stmt) {
        begin_paren("struct");
        add_identifier(stmt.name);
        inc_indent(); newline();
        for (auto& field : stmt.fields) {
            begin_paren();
            add_identifier(field.name);
            if (Type* type = field.type.get()) {
                visit(*type);
            }
            end_paren();
        }
        end_paren(); dec_indent();
    }

    void visit_impl(FunctionStmt& stmt) {
        begin_paren("fun");
        add_identifier(stmt.name);
        for (auto& field : stmt.params) {
            begin_paren();
            add_identifier(field.name);
            if (Type* type = field.type.get()) {
                visit(*type);
            }

            end_paren();
        }

        if (stmt.ret_type.get()) visit(*stmt.ret_type);

        inc_indent();
        for (auto& stmt : stmt.body) {
            newline();
            visit(*stmt);
        }
        end_paren(); dec_indent();
    }

    void visit_impl(IfStmt& stmt) {
        begin_paren("if");
        visit(*stmt.condition);
        inc_indent(); newline();
        visit(*stmt.then_branch);
        if (Stmt* else_stmt = stmt.else_branch.get()) {
            newline();
            visit(*else_stmt);
        }
        end_paren(); dec_indent();
    }
    void visit_impl(PrintStmt& stmt) {
        begin_paren("print");
        visit(*stmt.expr);
        end_paren();
    }
    void visit_impl(VarStmt& stmt) {
        begin_paren("var");
        add_identifier(stmt.var.name);
        if (Type* type = stmt.var.type.get()) {
            visit(*type);
        }
        if (Expr* expr = stmt.initializer.get()) {
            visit(*expr);
        }
        end_paren();
    }
    void visit_impl(WhileStmt& stmt) {
        begin_paren("while");
        visit(*stmt.condition);
        visit(*stmt.body);
        end_paren();
    }
    void visit_impl(ReturnStmt& stmt) {
        begin_paren("return");
        if (Expr* expr = stmt.expr.get()) {
            visit(*expr);
        }
        end_paren();
    }
    void visit_impl(BreakStmt& stmt) {
        begin_paren("break");
        end_paren();
    }
    void visit_impl(ContinueStmt& stmt) {
        begin_paren("continue");
        end_paren();
    }

    void visit_impl(ErrorExpr& expr) {
        add_identifier("error");
    }
    void visit_impl(AssignExpr& expr) {
        begin_paren("set");
        if (expr.type.get()) visit(*expr.type);
        add_identifier(expr.name);
        visit(*expr.value);
        end_paren();
    }
    void visit_impl(BinaryExpr& expr) {
        begin_paren(get_token_str(expr.op));
        if (expr.type.get()) visit(*expr.type);
        visit(*expr.left);
        visit(*expr.right);
        end_paren();
    }
    void visit_impl(CallExpr& expr) {
        begin_paren("call");
        if (expr.type.get()) visit(*expr.type);
        visit(*expr.callee);
        for (auto& arg : expr.arguments) {
            visit(*arg);
        }
        end_paren();
    }
    void visit_impl(TernaryExpr& expr) {
        begin_paren("ternary");
        if (expr.type.get()) visit(*expr.type);
        visit(*expr.cond);
        visit(*expr.left);
        visit(*expr.right);
        end_paren();
    }
    void visit_impl(GroupingExpr& expr) {
        begin_paren("grouping");
        if (expr.type.get()) visit(*expr.type);
        visit(*expr.expression);
        end_paren();
    }
    void visit_impl(LiteralExpr& expr) {
        begin_paren("lit");
        if (expr.type.get()) visit(*expr.type);
        add_identifier(expr.value.to_std_string());
        end_paren();
    }
    void visit_impl(UnaryExpr& expr) {
        begin_paren(get_token_str(expr.op));
        if (expr.type.get()) visit(*expr.type);
        visit(*expr.right);
        end_paren();
    }
    void visit_impl(VariableExpr& expr) {
        if (expr.type.get()) {
            begin_paren();
            visit(*expr.type);
            add_identifier(expr.name);
            end_paren();
        }
        else {
            add_identifier(expr.name);
        }
    }

    void visit_impl(PrimitiveType& type) {
        switch (type.prim_kind) {
            case PrimTypeKind::Void: add_identifier("'void"); break;
            case PrimTypeKind::Bool: add_identifier("'bool"); break;
            case PrimTypeKind::U8: add_identifier("'u8"); break;
            case PrimTypeKind::U16: add_identifier("'u16"); break;
            case PrimTypeKind::U32: add_identifier("'u32"); break;
            case PrimTypeKind::U64: add_identifier("'u64"); break;
            case PrimTypeKind::I8: add_identifier("'i8"); break;
            case PrimTypeKind::I16: add_identifier("'i16"); break;
            case PrimTypeKind::I32: add_identifier("'i32"); break;
            case PrimTypeKind::I64: add_identifier("'i64"); break;
            case PrimTypeKind::F32: add_identifier("'f32"); break;
            case PrimTypeKind::F64: add_identifier("'f64"); break;
            case PrimTypeKind::String: add_identifier("'string"); break;
        }
    }
    void visit_impl(StructType& type) {
        begin_paren("struct");
        add_identifier(type.name);
        inc_indent(); newline();
        for (auto& var_decl : type.decl) {
            begin_paren();
            add_identifier(var_decl.name);
            visit(*var_decl.type);
            end_paren();
            newline();
        }
        end_paren(); dec_indent();
    }
    void visit_impl(FunctionType& type) {
        begin_paren("fun");
        for (auto& param_type : type.params) {
            visit(*param_type);
        }
        add_identifier("->");
        visit(*type.ret);
        end_paren();
    }

private:
    const u8* m_source;
    const char* m_tab_chars = "    ";
    u32 m_tab_count = 0;
    std::string m_buf;

    void begin_paren() { m_buf += '('; }
    void begin_paren(std::string_view name) { m_buf += '('; m_buf += name; m_buf += ' '; }
    void end_paren() {
        if (m_buf[m_buf.size() - 1] == ' ') {
            m_buf[m_buf.size() - 1] = ')';
            m_buf += ' ';
        }
        else {
            m_buf += ") ";
        }
    }
    void add_identifier(std::string_view identifier) { m_buf += identifier; m_buf += ' '; }
    void add_identifier(Token token) { m_buf += get_token_str(token); m_buf += ' '; }

    void inc_indent() { m_tab_count++; }
    void dec_indent() { m_tab_count--; }
    void indent() {
        for (u32 i = 0; i < m_tab_count; i++) m_buf += m_tab_chars;
    }
    void newline() {
        m_buf += '\n';
        indent();
    }
};

}
#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/token.hpp"

#include <string>

namespace rx {

class Expr;
class Stmt;

class AstPrinter {
private:
    const char* m_tab_chars = "    ";
    u32 m_tab_count = 0;
    std::string m_buf;

public:
    std::string to_string(const Expr* expr);
    std::string to_string(const Vector<Stmt*>& statements);

    void add(const Expr* expr);
    void add(const Stmt* stmt);

private:
    void inc_indent() { m_tab_count++; }
    void dec_indent() { m_tab_count--; }
    void indent() {
        for (u32 i = 0; i < m_tab_count; i++) m_buf += m_tab_chars;
    }
    void newline() {
        m_buf += '\n';
        indent();
    }
    void add(std::string_view str) {
        m_buf += str;
    }
    void add(const Vector<Expr*>& expressions) {
        for (u32 i = 0; i < expressions.size() - 1; i++) {
            add(expressions[i]);
            m_buf += ' ';
        }
        add(expressions[expressions.size() - 1]);
    }
    void add(const Vector<Stmt*>& statements) {
        inc_indent();
        for (u32 i = 0; i < statements.size(); i++) {
            newline();
            add(statements[i]);
        }
        dec_indent();
    }
    void add(const Vector<Token>& tokens) {
        for (u32 i = 0; i < tokens.size() - 1; i++) {
            m_buf += tokens[i].str();
            m_buf += ' ';
        }
        m_buf += tokens[tokens.size() - 1].str();
    }
    void reset() {
        m_tab_count = 0;
        m_buf.clear();
    }

    std::string to_string(std::string_view str) { return std::string(str); }

    template <typename ... ExprT>
    void parenthesize(std::string_view name, ExprT&&... exprs) {
        m_buf += "(";
        m_buf += name;
        (((m_buf += ' ', add(std::forward<ExprT>(exprs)))),...);
        m_buf += ")";
    }

    void parenthesize_block(std::string_view name, const Vector<Stmt*>& statements);
};

}
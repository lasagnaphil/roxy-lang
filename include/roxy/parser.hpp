#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/fmt/core.h"
#include "roxy/scanner.hpp"
#include "roxy/value.hpp"
#include "roxy/expr.hpp"
#include "roxy/string_interner.hpp"

#include <utility>
#include <cassert>
#include <type_traits>
#include <string>
#include <string_view>

namespace rx {

enum class Precedence {
    None,
    Assignment,
    Ternary,
    Or,
    And,
    Equality,
    Comparison,
    Term,
    Factor,
    Unary,
    Call,
    Primary,
    _count
};

class Parser;

using PrefixParseFn = Expr* (Parser::*)(bool can_assign);
using InfixParseFn = Expr* (Parser::*)(bool can_assign, Expr* left);

struct ParseRule {
    PrefixParseFn prefix_fn;
    InfixParseFn infix_fn;
    Precedence precedence;
};

class Parser {
private:
    static constexpr u64 s_initial_expr_buffer_capacity = 65536;
    static ParseRule s_parse_rules[];

    Scanner* m_scanner;
    BumpAllocator m_expr_allocator;
    Token m_previous = {}, m_current = {};
    bool m_had_error = false;
    bool m_panic_mode = false;

    StringInterner* m_string_interner;

public:
    Parser(Scanner* scanner, StringInterner* string_interner) :
        m_scanner(scanner), m_string_interner(string_interner),
        m_expr_allocator(s_initial_expr_buffer_capacity) {

    }

    Parser(const Parser& compiler) = delete;
    Parser& operator=(const Parser& compiler) = delete;
    Parser(Parser&& compiler) = delete;
    Parser& operator=(Parser&& compiler) = delete;

    template <typename ExprT, typename ... Args, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT* new_expr(Args&&... args) {
        return m_expr_allocator.emplace<ExprT>(std::forward<Args>(args)...);
    }

    Expr* parse() {
        advance();
        return expression();
    }

private:

    Expr* expression() {
        return parse_precedence(Precedence::Assignment);
    }

    Expr* grouping(bool can_assign) {
        Expr* expr = expression();
        if (!consume(TokenType::RightParen)) {
            error_at_current("Expect ')' after expression.");
            return new_expr<ErrorExpr>();
        }
        return new_expr<GroupingExpr>(expr);
    }

    Expr* number(bool can_assign) {
        double value = strtod(reinterpret_cast<const char*>(previous().start), nullptr);
        return new_expr<LiteralExpr>(Value(value));
    }

    Expr* string(bool can_assign) {
        ObjString* str = m_string_interner->create_string(
                reinterpret_cast<const char*>(previous().start + 1),
                (u32)(previous().length - 2));
        auto value = Value(str);
        value.obj_incref();
        return new_expr<LiteralExpr>(Value(str));
    }

    Expr* literal(bool can_assign) {
        switch (previous().type) {
            case TokenType::False: return new_expr<LiteralExpr>(Value(false));
            case TokenType::True: return new_expr<LiteralExpr>(Value(true));
            case TokenType::Nil: return new_expr<LiteralExpr>(Value());
            default: return new_expr<ErrorExpr>();
        }
    }

    Expr* table(bool can_assign) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* array(bool can_assign) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* variable(bool can_assign) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }


    Expr* super(bool can_assign) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* this_(bool can_assign) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* unary(bool can_assign) {
        Token op = previous();
        Expr* expr = parse_precedence(Precedence::Unary);
        return new_expr<UnaryExpr>(op, expr);
    }

    Expr* binary(bool can_assign, Expr* left) {
        Token op = previous();
        ParseRule rule = get_rule(op.type);
        Expr* right = parse_precedence((Precedence)((u32)rule.precedence + 1));
        return new_expr<BinaryExpr>(left, op, right);
    }

    Expr* call(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* subscript(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* dot(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* and_(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* or_(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* ternary(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* parse_precedence(Precedence precedence) {
#define MEMBER_FN(object,ptrToMember)  ((object).*(ptrToMember))
        advance();
        TokenType prefix_type = previous().type;
        PrefixParseFn prefix_rule = get_rule(prefix_type).prefix_fn;
        if (prefix_rule == nullptr) {
            error("Expect expression.");
            return new_expr<ErrorExpr>();
        }

        bool can_assign = precedence <= Precedence::Assignment;
        Expr* expr = MEMBER_FN(*this, prefix_rule)(can_assign);

        while (precedence <= get_rule(current().type).precedence) {
            advance();
            InfixParseFn infix_rule = get_rule(previous().type).infix_fn;
            expr = MEMBER_FN(*this, infix_rule)(can_assign, expr);
        }

        if (can_assign && match(TokenType::Equal)) {
            error("Invalid assignment target.");
            return new_expr<ErrorExpr>();
        }
        return expr;

#undef MEMBER_FN
    }

    ParseRule get_rule(TokenType type) const {
        return s_parse_rules[(u32)type];
    }

    void advance() {
        m_previous = m_current;
        for (;;) {
            m_current = m_scanner->scan_token();
            if (m_current.type != TokenType::Error) break;

            error_at_current(reinterpret_cast<const char*>(m_current.start));
        }
    }

    bool consume(TokenType type) {
        if (m_current.type == type) {
            advance();
            return true;
        }
        return false;
    }

    bool check(TokenType type) const {
        return m_current.type == type;
    }

    bool match(TokenType type) {
        if (check(type)) {
            advance();
            return true;
        }
        return false;
    }

    bool match(std::initializer_list<TokenType> types) {
        for (TokenType type : types) {
            if (check(type)) {
                advance();
                return true;
            }
        }
        return false;
    }

    void synchronize() {
        m_panic_mode = false;

        while (m_current.type != TokenType::Eof) {
            if (m_previous.type == TokenType::Semicolon) return;
            switch (m_current.type) {;
                case TokenType::Class:
                case TokenType::Fun:
                case TokenType::Var:
                case TokenType::If:
                case TokenType::While:
                case TokenType::Return:
                    return;

                default:
                    ;
            }
            advance();
        }
    }

    void error_at_current(const char* message) {
        error_at(m_current, message);
    }

    void error(const char* message) {
        error_at(m_previous, message);
    }

    void error_at(const Token& token, const char* message) {
        if (m_panic_mode) return;
        m_panic_mode = true;
        fmt::print(stderr, "[line {}] Error", token.line);
        if (token.type == TokenType::Eof) {
            fmt::print(stderr, " at end");
        }
        else if (token.type == TokenType::Error) {
            // Nothing.
        }
        else {
            fmt::print(stderr, " at '{}'", token.str());
        }

        fmt::print(stderr, ": {}\n", message);
        m_had_error = true;
    }

    void error_unimplemented() {
        error_at(m_previous, "Unimplemented feature!");
    }

    void reset_errors() {
        m_had_error = false;
        m_panic_mode = false;
    }

    Token previous() const { return m_previous; }
    Token current() const { return m_current; }
};

}
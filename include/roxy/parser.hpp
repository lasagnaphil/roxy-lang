#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/fmt/core.h"
#include "roxy/scanner.hpp"
#include "roxy/value.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/string_interner.hpp"
#include "ast_printer.hpp"

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
    BumpAllocator m_allocator;
    Token m_previous = {}, m_current = {};
    bool m_had_error = false;
    bool m_panic_mode = false;

    StringInterner* m_string_interner;

public:
    Parser(Scanner* scanner, StringInterner* string_interner) :
            m_scanner(scanner), m_string_interner(string_interner),
            m_allocator(s_initial_expr_buffer_capacity) {

    }

    Parser(const Parser& compiler) = delete;
    Parser& operator=(const Parser& compiler) = delete;
    Parser(Parser&& compiler) = delete;
    Parser& operator=(Parser&& compiler) = delete;

    template <typename ExprT, typename ... Args, typename = std::enable_if_t<std::is_base_of_v<Expr, ExprT>>>
    ExprT* new_expr(Args&&... args) {
        return m_allocator.emplace<ExprT>(std::forward<Args>(args)...);
    }

    template <typename StmtT, typename ... Args, typename = std::enable_if_t<std::is_base_of_v<Stmt, StmtT>>>
    StmtT* new_stmt(Args&&... args) {
        return m_allocator.emplace<StmtT>(std::forward<Args>(args)...);
    }

    template <typename TypeT, typename ... Args, typename = std::enable_if_t<std::is_base_of_v<Type, TypeT>>>
    TypeT* new_type(Args&&... args) {
        return m_allocator.emplace<TypeT>(std::forward<Args>(args)...);
    }

    void parse(Vector<Stmt*>& statements) {
        advance();
        while (!m_scanner->is_at_end()) {
            statements.push_back(declaration());
        }
    }

private:

    ErrorExpr* error_expr(std::string_view message) {
        error_at_current(message);
        return new_expr<ErrorExpr>();
    }

    ErrorStmt* error_stmt(std::string_view message) {
        error_at_current(message);
        return new_stmt<ErrorStmt>();
    }

    Expr* expression() {
        return parse_precedence(Precedence::Assignment);
    }

    Vector<Stmt*> block() {
        Vector<Stmt*> statements;
        while (!check(TokenType::RightBrace) && !check(TokenType::Eof)) {
            statements.push_back(declaration());
        }
        if (!consume(TokenType::RightBrace)) {
            return {error_stmt("Expect '}' after block.")};
        }
        return statements;
    }

    Stmt* declaration() {
        if (match(TokenType::Var)) {
            return var_declaration();
        }
        else if (match(TokenType::Fun)) {
            return fun_declaration();
        }
        else if (match(TokenType::Struct)) {
            return struct_declaration();
        }
        else {
            return statement();
        }
    }

    Stmt* statement() {
        Stmt* stmt;
        if (match(TokenType::LeftBrace)) {
            stmt = new_stmt<BlockStmt>(block());
        }
        else if (match(TokenType::If)) {
            stmt = if_statement();
        }
        else if (match(TokenType::Print)) {
            stmt = print_statement();
        }
        else if (match(TokenType::While)) {
            stmt = while_statement();
        }
        else if (match(TokenType::For)) {
            stmt = for_statement();
        }
        else if (match(TokenType::Return)) {
            stmt = return_statement();
        }
        else if (match(TokenType::Break)) {
            stmt = break_statement();
        }
        else if (match(TokenType::Continue)) {
            stmt = continue_statement();
        }
        else {
            stmt = expression_statement();
        }
        if (m_panic_mode) synchronize();
        return stmt;
    }

    Stmt* if_statement() {
        if (!consume(TokenType::LeftParen)) {
            return error_stmt("Expect '(' after 'if'.");
        }
        Expr* condition = expression();
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expect ')' after if condition.");
        }

        Stmt* then_branch = statement();
        Stmt* else_branch;
        if (match(TokenType::Else)) {
            else_branch = statement();
        }
        else{
            else_branch = nullptr;
        }
        return new_stmt<IfStmt>(condition, then_branch, else_branch);
    }

    Stmt* print_statement() {
        Expr* value = expression();
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after value.");
        }
        return new_stmt<PrintStmt>(value);
    }

    Stmt* while_statement() {
        if (!consume(TokenType::LeftParen)) {
            return error_stmt("Expect '(' after 'while'.");
        }
        Expr* condition = expression();
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expext ')' after condition.");
        }
        Stmt* body = statement();
        return new_stmt<WhileStmt>(condition, body);
    }

    Stmt* for_statement() {
        if (!consume(TokenType::LeftParen)) {
            return error_stmt("Expect '(' after 'for'.");
        }

        Stmt* initializer;
        if (match(TokenType::Semicolon)) {
            initializer = nullptr;
        }
        else if (match(TokenType::Var)) {
            initializer = var_declaration();
        }
        else {
            initializer = expression_statement();
        }

        Expr* condition = nullptr;
        if (!check(TokenType::Semicolon)) {
            condition = expression();
        }
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after loop condition.");
        }

        Expr* increment = nullptr;
        if (!check(TokenType::RightParen)) {
            increment = expression();
        }
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expect ')' after for clauses.");
        }

        Stmt* body = statement();

        if (increment != nullptr) {
            Vector<Stmt*> stmts = {body, new_stmt<ExpressionStmt>(increment)};
            body = new_stmt<BlockStmt>(std::move(stmts));
        }

        if (condition != nullptr) {
            condition = new_expr<LiteralExpr>(Value(true));
        }
        body = new_stmt<WhileStmt>(condition, body);

        if (initializer != nullptr) {
            Vector<Stmt*> stmts = {initializer, body};
            body = new_stmt<BlockStmt>(std::move(stmts));
        }

        return body;
    }

    Stmt* return_statement() {
        // TODO: check if this is top-level code

        if (match(TokenType::Semicolon)) {
            return new_stmt<ReturnStmt>(nullptr);
        }
        else {
            Expr* expr = expression();
            if (!consume(TokenType::Semicolon)) {
                return error_stmt("Expect ';' after return value.");
            }
            return new_stmt<ReturnStmt>(expr);
        }
    }

    Stmt* break_statement() {
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after 'break'.");
        }
        return new_stmt<BreakStmt>();
    }

    Stmt* continue_statement() {
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after 'break'.");
        }
        return new_stmt<ContinueStmt>();
    }

    // TODO: Make this much faster
    bool parse_primitive_type(std::string_view name, PrimTypeKind& prim_kind) {
        if (name == "bool") {
            prim_kind = PrimTypeKind::Bool;
        }
        else if (name == "number") {
            prim_kind = PrimTypeKind::Number;
        }
        else if (name == "string") {
            prim_kind = PrimTypeKind::String;
        }
        else {
            return false;
        }
        return true;
    }

    bool parse_variable(const char* var_kind, std::string& err_msg, VarDecl& variable) {
        if (!consume(TokenType::Identifier)) {
            err_msg = fmt::format("Expect {} name.", var_kind);
            return false;
        }
        Token name = previous();
        Type* type = nullptr;
        if (match(TokenType::Colon)) {
            if (!consume(TokenType::Identifier)) {
                err_msg = "Expect type name.";
                return false;
            }
            Token type_name = previous();
            PrimTypeKind prim_kind;
            if (!parse_primitive_type(type_name.str(), prim_kind)) {
                err_msg = "Invalid type name.";
                return false;
            }
            type= new_type<PrimitiveType>(prim_kind);
        }
        variable = VarDecl(name, type);
        return true;
    }

    Stmt* var_declaration() {
        VarDecl var_decl;
        std::string err_msg;
        if (!parse_variable("variable", err_msg, var_decl)) {
            return error_stmt(err_msg);
        }

        Expr* initializer = nullptr;
        if (match(TokenType::Equal)) {
            initializer = expression();
        }

        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after variable declaration.");
        }
        return new_stmt<VarStmt>(var_decl, initializer);
    }

    Stmt* fun_declaration() {
        if (!consume(TokenType::Identifier)) {
            return error_stmt("Expect function name.");
        }
        Token name = previous();
        Vector<VarDecl> parameters;
        if (!consume(TokenType::LeftParen)) {
            return error_stmt("Expect '(' after function name.");
        }
        if (!check(TokenType::RightParen)) {
            do {
                if (parameters.size() >= 255) {
                    return error_stmt("Can't have more than 255 parameters.");
                }
                VarDecl var_decl;
                std::string err_msg;
                if (!parse_variable("parameter", err_msg, var_decl)) {
                    return error_stmt(err_msg);
                }
                parameters.push_back(var_decl);
            } while (match(TokenType::Comma));
        }
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expect ')' after parameters.");
        }

        if (!consume(TokenType::LeftBrace)) {
            return error_stmt("Expect '{' before function body.");
        }
        Vector<Stmt*> body = block();
        return new_stmt<FunctionStmt>(name, std::move(parameters), std::move(body));
    }

    Stmt* struct_declaration() {
        if (!consume(TokenType::Identifier)) {
            return error_stmt("Expect struct name.");
        }
        Token name = previous();

        if (!consume(TokenType::LeftBrace)) {
            return error_stmt("Expect '{' before struct body.");
        }

        Vector<VarDecl> fields;
        while (!check(TokenType::RightBrace) && !m_scanner->is_at_end()){
            VarDecl var_decl;
            std::string err_msg;
            if (!parse_variable("field", err_msg, var_decl)) {
                return error_stmt(err_msg);
            }
            fields.push_back(var_decl);
        }

        if (!consume(TokenType::RightBrace)) {
            return error_stmt("Expect '}' after class body.");
        }

        return new_stmt<StructStmt>(name, std::move(fields));
    }

    Stmt* expression_statement() {
        Expr* expr = expression();
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after expression.");
        }
        return new_stmt<ExpressionStmt>(expr);
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

    Expr* named_variable(Token name, bool can_assign) {
        if (can_assign && match(TokenType::Equal)) {
            Expr* value = expression();
            return new_expr<AssignExpr>(name, value);
        }
        else {
            return new_expr<VariableExpr>(name);
        }
    }

    Expr* variable(bool can_assign) {
        return named_variable(previous(), can_assign);
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
        Vector<Expr*> arguments;
        if (!check(TokenType::RightParen)) {
            do {
                if (arguments.size() == 255) {
                    return error_expr("Can't have more than 255 arguments.");
                }
                arguments.push_back(expression());
            } while (match(TokenType::Comma));
        }
        if (!consume(TokenType::RightParen)) {
            return error_expr("Expect ')' after arguments.");
        }
        Token paren = previous();
        return new_expr<CallExpr>(left, paren, std::move(arguments));
    }

    Expr* subscript(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* dot(bool can_assign, Expr* left) {
        error_unimplemented();
        return new_expr<ErrorExpr>();
    }

    Expr* logical_and(bool can_assign, Expr* left) {
        Token op = previous();
        Expr* right = parse_precedence(Precedence::And);
        return new_expr<BinaryExpr>(left, op, right);
    }

    Expr* logical_or(bool can_assign, Expr* left) {
        Token op = previous();
        Expr* right = parse_precedence(Precedence::Or);
        return new_expr<BinaryExpr>(left, op, right);
    }

    Expr* ternary(bool can_assign, Expr* cond) {
        Expr* left = parse_precedence(Precedence::Ternary);
        if (!consume(TokenType::Colon)) {
            return error_expr("Expect ':' after expression.");
        }
        Expr* right = parse_precedence(Precedence::Ternary);
        return new_expr<TernaryExpr>(cond, left, right);
    }

    Expr* parse_precedence(Precedence precedence) {
#define MEMBER_FN(object,ptrToMember)  ((object).*(ptrToMember))
        advance();
        TokenType prefix_type = previous().type;
        PrefixParseFn prefix_rule = get_rule(prefix_type).prefix_fn;
        if (prefix_rule == nullptr) {
            return error_expr("Expect expression.");
        }

        bool can_assign = precedence <= Precedence::Assignment;
        Expr* expr = MEMBER_FN(*this, prefix_rule)(can_assign);

        while (precedence <= get_rule(current().type).precedence) {
            advance();
            InfixParseFn infix_rule = get_rule(previous().type).infix_fn;
            expr = MEMBER_FN(*this, infix_rule)(can_assign, expr);
        }

        if (can_assign && match(TokenType::Equal)) {
            return error_expr("Invalid assignment target.");
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

    bool match_multiple(std::initializer_list<TokenType> types) {
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
                case TokenType::Struct:
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

    void error_at_current(std::string_view message) {
        error_at(m_current, message);
    }

    void error(const char* message) {
        error_at(m_previous, message);
    }

    void error_at(const Token& token, std::string_view message) {
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
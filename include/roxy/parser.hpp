#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/fmt/core.h"
#include "roxy/scanner.hpp"
#include "roxy/value.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/string_interner.hpp"
#include "roxy/ast_allocator.hpp"
#include "roxy/ast_printer.hpp"
#include "roxy/sema.hpp"

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
    static constexpr u64 s_initial_ast_allocator_capacity = 65536;
    static ParseRule s_parse_rules[];

    Scanner* m_scanner;
    Token m_previous = {}, m_current = {};
    bool m_had_error = false;
    bool m_panic_mode = false;

    AstAllocator m_allocator;
    SemaAnalyzer m_sema_analyzer;
    StringInterner* m_string_interner;

public:
    Parser(Scanner* scanner, StringInterner* string_interner) :
            m_scanner(scanner),
            m_allocator(s_initial_ast_allocator_capacity),
            m_sema_analyzer(&m_allocator, m_scanner->source()),
            m_string_interner(string_interner) {

    }

    Parser(const Parser& parser) = delete;
    Parser& operator=(const Parser& parser) = delete;
    Parser(Parser&& parser) = delete;
    Parser& operator=(Parser&& parser) = delete;

    template <typename T, typename ... Args>
    T* alloc(Args&&... args) {
        return m_allocator.alloc<T, Args...>(std::forward<Args>(args)...);
    }

    template <typename T>
    Span<T> alloc_vector(Vector<T>&& vec) {
        return m_allocator.alloc_vector<T, T>(std::move(vec));
    }

    template <typename T>
    Span<RelPtr<T>> alloc_vector_ptr(Vector<T*>&& vec) {
        return m_allocator.alloc_vector<RelPtr<T>, T*>(std::move(vec));
    }

    Span<AstVarDecl> alloc_vector_var_decl(Vector<VarDecl>&& vec) {
        return m_allocator.alloc_vector<AstVarDecl, VarDecl>(std::move(vec));
    }

    AstAllocator* get_ast_allocator() { return &m_allocator; }

    bool parse(BlockStmt*& stmt) {
        advance();
        Vector<Stmt*> statements;
        while (!m_scanner->is_at_end()) {
            statements.push_back(declaration());
        }
        auto alloc_statements = alloc_vector_ptr(std::move(statements));
        stmt = alloc<BlockStmt>(alloc_statements);
        return !m_had_error;
    }

private:

    std::string_view get_token_str(Token token) const {
        return token.str(m_scanner->source());
    }

    ErrorExpr* error_expr(std::string_view message) {
        error_at_current(message);
        return alloc<ErrorExpr>();
    }

    ErrorStmt* error_stmt(std::string_view message) {
        error_at_current(message);
        return alloc<ErrorStmt>();
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
            stmt = alloc<BlockStmt>(alloc_vector_ptr(block()));
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
        auto condition = expression();
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expect ')' after if condition.");
        }

        auto then_branch = statement();
        auto else_branch = match(TokenType::Else)? statement() : nullptr;
        return alloc<IfStmt>(condition, then_branch, else_branch);
    }

    Stmt* print_statement() {
        auto value = expression();
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after value.");
        }
        return alloc<PrintStmt>(value);
    }

    Stmt* while_statement() {
        if (!consume(TokenType::LeftParen)) {
            return error_stmt("Expect '(' after 'while'.");
        }
        auto condition = expression();
        if (!consume(TokenType::RightParen)) {
            return error_stmt("Expext ')' after condition.");
        }
        auto body = statement();
        return alloc<WhileStmt>(condition, body);
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
            Vector<Stmt*> stmts = {body, alloc<ExpressionStmt>(increment)};
            body = alloc<BlockStmt>(alloc_vector_ptr(std::move(stmts)));
        }

        if (condition == nullptr) {
            condition = alloc<LiteralExpr>(AnyValue(true));
        }
        body = alloc<WhileStmt>(condition, body);

        if (initializer != nullptr) {
            Vector<Stmt*> stmts = {initializer, body};
            body = alloc<BlockStmt>(alloc_vector_ptr(std::move(stmts)));
        }

        return body;
    }

    Stmt* return_statement() {
        // TODO: check if this is top-level code

        if (match(TokenType::Semicolon)) {
            return alloc<ReturnStmt>(nullptr);
        }
        else {
            Expr* expr = expression();
            if (!consume(TokenType::Semicolon)) {
                return error_stmt("Expect ';' after return value.");
            }
            return alloc<ReturnStmt>(expr);
        }
    }

    Stmt* break_statement() {
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after 'break'.");
        }
        return alloc<BreakStmt>();
    }

    Stmt* continue_statement() {
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after 'break'.");
        }
        return alloc<ContinueStmt>();
    }

    // TODO: Make this much faster
    bool parse_primitive_type(std::string_view name, PrimTypeKind& prim_kind, bool include_void = false) {
        if (include_void) {
            if (name == "void") {
                prim_kind = PrimTypeKind::Void;
                return true;
            }
        }
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
            auto type_str = get_token_str(type_name);
            if (!parse_primitive_type(type_str, prim_kind)) {
                err_msg = "Invalid type name.";
                return false;
            }
            type = alloc<PrimitiveType>(prim_kind);
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

        if (var_decl.type == nullptr && initializer == nullptr) {
            return error_stmt("Expect explicit kind for var declaration.");
        }

        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after variable declaration.");
        }
        return alloc<VarStmt>(var_decl, initializer);
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
        Type* ret_type = nullptr;
        if (consume(TokenType::Colon)) {
            if (!consume(TokenType::Identifier)) {
                return error_stmt("Expect type after ':'.");
            }
            auto ret_type_name = get_token_str(previous());
            PrimTypeKind prim_type_kind;
            if (!parse_primitive_type(ret_type_name, prim_type_kind, true)) {
                return error_stmt("Expect valid type after ':'");
            }
            ret_type = alloc<PrimitiveType>(prim_type_kind);
        }
        else {
            // Default return type is void.
            ret_type = alloc<PrimitiveType>(PrimTypeKind::Void);
        }

        if (!consume(TokenType::LeftBrace)) {
            return error_stmt("Expect '{' before function body.");
        }
        return alloc<FunctionStmt>(name,
                                   alloc_vector_var_decl(std::move(parameters)),
                                   alloc_vector_ptr(block()),
                                   ret_type);
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

        return alloc<StructStmt>(name, alloc_vector_var_decl(std::move(fields)));
    }

    Stmt* expression_statement() {
        Expr* expr = expression();
        if (!consume(TokenType::Semicolon)) {
            return error_stmt("Expect ';' after expression.");
        }
        return alloc<ExpressionStmt>(expr);
    }

    Expr* grouping(bool can_assign) {
        Expr* expr = expression();
        if (!consume(TokenType::RightParen)) {
            error_at_current("Expect ')' after expression.");
            return alloc<ErrorExpr>();
        }
        return alloc<GroupingExpr>(expr);
    }

    Expr* number(bool can_assign) {
        double value = strtod(reinterpret_cast<const char*>(m_scanner->source() + previous().source_loc), nullptr);
        return alloc<LiteralExpr>(AnyValue(value));
    }

    Expr* string(bool can_assign) {
        std::string_view contents = get_token_str(previous()).substr(1, previous().length - 2);
        ObjString* str = m_string_interner->create_string(contents);
        auto value = AnyValue(str);
        value.obj_incref();
        return alloc<LiteralExpr>(AnyValue(str));
    }

    Expr* literal(bool can_assign) {
        switch (previous().type) {
            case TokenType::False: return alloc<LiteralExpr>(AnyValue(false));
            case TokenType::True: return alloc<LiteralExpr>(AnyValue(true));
            case TokenType::Nil: return alloc<LiteralExpr>(AnyValue());
            default: return alloc<ErrorExpr>();
        }
    }

    Expr* table(bool can_assign) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* array(bool can_assign) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* named_variable(Token name, bool can_assign) {
        if (can_assign && match(TokenType::Equal)) {
            Expr* value = expression();
            return alloc<AssignExpr>(name, value);
        }
        else {
            return alloc<VariableExpr>(name);
        }
    }

    Expr* variable(bool can_assign) {
        return named_variable(previous(), can_assign);
    }

    Expr* super(bool can_assign) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* this_(bool can_assign) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* unary(bool can_assign) {
        Token op = previous();
        Expr* expr = parse_precedence(Precedence::Unary);
        return alloc<UnaryExpr>(op, expr);
    }

    Expr* binary(bool can_assign, Expr* left) {
        Token op = previous();
        ParseRule rule = get_rule(op.type);
        Expr* right = parse_precedence((Precedence)((u32)rule.precedence + 1));
        return alloc<BinaryExpr>(left, op, right);
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
        return alloc<CallExpr>(left, paren, alloc_vector_ptr(std::move(arguments)));
    }

    Expr* subscript(bool can_assign, Expr* left) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* dot(bool can_assign, Expr* left) {
        error_unimplemented();
        return alloc<ErrorExpr>();
    }

    Expr* logical_and(bool can_assign, Expr* left) {
        Token op = previous();
        Expr* right = parse_precedence(Precedence::And);
        return alloc<BinaryExpr>(left, op, right);
    }

    Expr* logical_or(bool can_assign, Expr* left) {
        Token op = previous();
        Expr* right = parse_precedence(Precedence::Or);
        return alloc<BinaryExpr>(left, op, right);
    }

    Expr* ternary(bool can_assign, Expr* cond) {
        Expr* left = parse_precedence(Precedence::Ternary);
        if (!consume(TokenType::Colon)) {
            return error_expr("Expect ':' after expression.");
        }
        Expr* right = parse_precedence(Precedence::Ternary);
        return alloc<TernaryExpr>(cond, left, right);
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
            if (!m_current.is_error()) break;

            error_at_current(get_token_str(m_current));
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
        fmt::print(stderr, "[line {}] Error", m_scanner->get_line(token));
        if (token.type == TokenType::Eof) {
            fmt::print(stderr, " at end");
        }
        else if (token.is_error()) {
            // Nothing.
        }
        else {
            fmt::print(stderr, " at '{}'", get_token_str(token));
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
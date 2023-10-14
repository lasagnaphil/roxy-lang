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
    bool m_inside_fun = false;

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

    SourceLocation get_previous_token_loc() const {
        return {m_previous.source_loc, m_previous.length};
    }

    SourceLocation get_current_token_loc() const {
        return {m_previous.source_loc, m_previous.length};
    }

    ErrorExpr* error_expr(SourceLocation loc, std::string_view message) {
        error_at_current(message);
        return alloc<ErrorExpr>(loc, message);
    }

    ErrorStmt* error_stmt(std::string_view message) {
        error_at_current(message);
        return alloc<ErrorStmt>(message);
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

        SourceLocation condition_loc;
        Expr* condition;
        if (check(TokenType::Semicolon)) {
            condition = nullptr;
            condition_loc = {get_previous_token_loc().source_loc, 0};
        }
        else {
            u32 start_loc = get_current_token_loc().source_loc;
            condition = expression();
            u32 end_loc = get_current_token_loc().source_loc;
            condition_loc = SourceLocation::from_start_end(start_loc, end_loc);
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
            condition = alloc<LiteralExpr>(condition_loc, AnyValue(true));
        }
        body = alloc<WhileStmt>(condition, body);

        if (initializer != nullptr) {
            Vector<Stmt*> stmts = {initializer, body};
            body = alloc<BlockStmt>(alloc_vector_ptr(std::move(stmts)));
        }

        return body;
    }

    Stmt* return_statement() {
        if (!m_inside_fun) {
            return error_stmt("Cannot return in top-level code.");
        }
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

    static bool parse_primitive_type(std::string_view name, PrimTypeKind& prim_kind, bool include_void = false) {
        switch (name[0]) {
            case 'v':
                if (include_void && name.size() == 4 && name.substr(1, 3) == "oid") {
                    prim_kind = PrimTypeKind::Void; return true;
                }
                break;
            case 'b':
                if (name.size() == 4 && name.substr(1, 3) == "ool") {
                    prim_kind = PrimTypeKind::Bool; return true;
                }
                break;
            case 's':
                if (name.size() == 6 && name.substr(1, 5) == "tring") {
                    prim_kind = PrimTypeKind::String; return true;
                }
                break;
            case 'i': case 'u':
                if (name.size() == 2 && name[1] == '8') {
                    prim_kind = name[0] == 'i'? PrimTypeKind::I8 : PrimTypeKind::U8; return true;
                }
                else if (name.size() == 3) {
                    if (name == "int") {
                        prim_kind = PrimTypeKind::I32; return true;
                    }
                    if (name[1] == '1' && name[2] == '6') {
                        prim_kind = name[0] == 'i'? PrimTypeKind::I16 : PrimTypeKind::U16; return true;
                    }
                    else if (name[1] == '3' && name[2] == '2') {
                        prim_kind = name[0] == 'i'? PrimTypeKind::I32: PrimTypeKind::U32; return true;
                    }
                    else if (name[1] == '6' && name[2] == '4') {
                        prim_kind = name[0] == 'i'? PrimTypeKind::I64: PrimTypeKind::U64; return true;
                    }
                }
                else if (name.size() == 4) {
                    if (name == "uint") {
                        prim_kind = PrimTypeKind::U32; return true;
                    }
                }
                break;
            case 'f':
                if (name.size() == 3) {
                    if (name[1] == '3' && name[2] == '2') {
                        prim_kind = PrimTypeKind::F32; return true;
                    }
                    else if (name[1] == '6' && name[2] == '4') {
                        prim_kind = PrimTypeKind::F64; return true;
                    }
                }
                else if (name.size() == 5) {
                    if (name.substr(1, 4) == "loat") {
                        prim_kind = PrimTypeKind::F32; return true;
                    }
                }
                break;
        }
        if (include_void) {
            if (name == "void") {
                prim_kind = PrimTypeKind::Void;
                return true;
            }
        }
        return false;
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
            if (parse_primitive_type(type_str, prim_kind)) {
                type = alloc<PrimitiveType>(prim_kind);
            }
            else {
                type = alloc<UnassignedType>(type_name);
            }
        }
        else {
            type = alloc<InferredType>();
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

        if (var_decl.type->kind == TypeKind::Inferred && initializer == nullptr) {
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
            auto ret_type_name = previous();
            auto ret_type_str = get_token_str(ret_type_name);
            PrimTypeKind prim_type_kind;
            if (parse_primitive_type(ret_type_str, prim_type_kind, true)) {
                ret_type = alloc<PrimitiveType>(prim_type_kind);
            }
            else {
                ret_type = alloc<UnassignedType>(ret_type_name);
            }
        }
        else {
            // Default return type is void.
            ret_type = alloc<PrimitiveType>(PrimTypeKind::Void);
        }

        if (!consume(TokenType::LeftBrace)) {
            return error_stmt("Expect '{' before function body.");
        }

        // TODO: what if there are functions inside functions?
        m_inside_fun = true;
        auto block_stmt_list = alloc_vector_ptr(block());
        m_inside_fun = false;

        return alloc<FunctionStmt>(name,
                                   alloc_vector_var_decl(std::move(parameters)),
                                   block_stmt_list,
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
        u32 start_loc = get_current_token_loc().source_loc;
        Expr* expr = expression();
        if (!consume(TokenType::RightParen)) {
            return error_expr(get_current_token_loc(), "Expect ')' after expression.");
        }
        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<GroupingExpr>(loc, expr);
    }

    Expr* number_i(bool can_assign) {
        auto str = std::string(get_token_str(previous()));
        auto token_loc = get_previous_token_loc();

        if (tolower(str[str.size() - 1]) == 'l') {
            if (tolower(str[str.size() - 2]) == 'u') {
                u64 value = std::stoull(str);
                return alloc<LiteralExpr>(token_loc, AnyValue(value));
            }
            else {
                i64 value = std::stoll(str);
                return alloc<LiteralExpr>(token_loc, AnyValue(value));
            }
        }
        else if (tolower(str[str.size() - 1]) == 'u') {
            u32 value = std::stoul(str);
            return alloc<LiteralExpr>(token_loc, AnyValue(value));
        }
        else {
            i32 value = std::stoi(str);
            return alloc<LiteralExpr>(token_loc, AnyValue(value));
        }
    }

    Expr* number_f(bool can_assign) {
        auto str = std::string(get_token_str(previous()));
        auto token_loc = get_previous_token_loc();

        if (tolower(str[str.size() - 1]) == 'f') {
            float value = std::stof(str);
            return alloc<LiteralExpr>(token_loc, AnyValue(value));
        }
        else {
            double value = std::stod(str);
            return alloc<LiteralExpr>(token_loc, AnyValue(value));
        }
    }

    Expr* string(bool can_assign) {
        std::string_view contents = get_token_str(previous()).substr(1, previous().length - 2);
        // TODO: store the string in a constant table
        ObjString* str = m_string_interner->create_string(contents);
        auto value = AnyValue(str->chars);
        return alloc<LiteralExpr>(get_previous_token_loc(), value);
    }

    Expr* literal(bool can_assign) {
        auto token_loc = get_previous_token_loc();
        switch (previous().type) {
            case TokenType::False: return alloc<LiteralExpr>(token_loc, AnyValue(false));
            case TokenType::True: return alloc<LiteralExpr>(token_loc, AnyValue(true));
            case TokenType::Nil: return alloc<LiteralExpr>(token_loc, AnyValue());
            default: return error_expr(get_current_token_loc(), "Unreachable code!");
        }
    }

    Expr* table(bool can_assign) {
        return error_expr(get_current_token_loc(), "Unimplemented!");
    }

    Expr* array(bool can_assign) {
        return error_expr(get_current_token_loc(), "Unimplemented!");
    }

    Expr* named_variable(Token name, bool can_assign) {
        auto start_loc = get_current_token_loc().source_loc;
        if (can_assign && match(TokenType::Equal)) {
            Expr* value = expression();
            auto end_loc = get_current_token_loc().source_loc;
            auto loc = SourceLocation {start_loc, (u16)(end_loc - start_loc)};
            return alloc<AssignExpr>(loc, name, value);
        }
        else {
            return alloc<VariableExpr>(name.get_source_loc(), name);
        }
    }

    Expr* variable(bool can_assign) {
        return named_variable(previous(), can_assign);
    }

    Expr* super(bool can_assign) {
        return error_expr(get_current_token_loc(), "Unimplemented!");
    }

    Expr* this_(bool can_assign) {
        return error_expr(get_current_token_loc(), "Unimplemented!");
    }

    Expr* unary(bool can_assign) {
        u32 start_loc = get_current_token_loc().source_loc;

        Token op = previous();
        Expr* expr = parse_precedence(Precedence::Unary);

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<UnaryExpr>(loc, op, expr);
    }

    Expr* binary(bool can_assign, Expr* left) {
        u32 start_loc = get_current_token_loc().source_loc;

        Token op = previous();
        ParseRule rule = get_rule(op.type);
        Expr* right = parse_precedence((Precedence)((u32)rule.precedence + 1));

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<BinaryExpr>(loc, left, op, right);
    }

    Expr* call(bool can_assign, Expr* left) {
        u32 start_loc = get_current_token_loc().source_loc;
        Vector<Expr*> arguments;
        if (!check(TokenType::RightParen)) {
            do {
                if (arguments.size() == 255) {
                    u32 end_loc = get_current_token_loc().source_loc;
                    auto loc = SourceLocation::from_start_end(start_loc, end_loc);
                    return error_expr(loc, "Can't have more than 255 arguments.");
                }
                arguments.push_back(expression());
            } while (match(TokenType::Comma));
        }
        if (!consume(TokenType::RightParen)) {
            return error_expr(get_current_token_loc(), "Expect ')' after arguments.");
        }
        Token paren = previous();

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<CallExpr>(loc, left, paren, alloc_vector_ptr(std::move(arguments)));
    }

    Expr* subscript(bool can_assign, Expr* left) {
        return error_expr(get_current_token_loc(), "Unimplemented!");
    }

    Expr* dot(bool can_assign, Expr* left) {
        u32 start_loc = get_current_token_loc().source_loc;
        if (!consume(TokenType::Identifier)) {
            return error_expr(get_current_token_loc(), "Expect property name after '.'.");
        }
        Token name = previous();
        if (can_assign && match(TokenType::Equal)) {
            Expr* right = expression();
            u32 end_loc = get_current_token_loc().source_loc;
            auto loc = SourceLocation::from_start_end(start_loc, end_loc);
            return alloc<SetExpr>(loc, left, name, right);
        }
        else if (match(TokenType::LeftParen)) {
            // TODO: Method calls
            return error_expr(get_current_token_loc(), "Unimplemented!");
        }
        else {
            u32 end_loc = get_current_token_loc().source_loc;
            auto loc = SourceLocation::from_start_end(start_loc, end_loc);
            return alloc<GetExpr>(loc, left, name);
        }
    }

    Expr* logical_and(bool can_assign, Expr* left) {
        u32 start_loc = get_current_token_loc().source_loc;

        Token op = previous();
        Expr* right = parse_precedence(Precedence::And);

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<BinaryExpr>(loc, left, op, right);
    }

    Expr* logical_or(bool can_assign, Expr* left) {
        u32 start_loc = get_current_token_loc().source_loc;

        Token op = previous();
        Expr* right = parse_precedence(Precedence::Or);

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<BinaryExpr>(loc, left, op, right);
    }

    Expr* ternary(bool can_assign, Expr* cond) {
        u32 start_loc = get_current_token_loc().source_loc;

        Expr* left = parse_precedence(Precedence::Ternary);
        if (!consume(TokenType::Colon)) {
            return error_expr(get_current_token_loc(), "Expect ':' after expression.");
        }
        Expr* right = parse_precedence(Precedence::Ternary);

        u32 end_loc = get_current_token_loc().source_loc;
        auto loc = SourceLocation::from_start_end(start_loc, end_loc);
        return alloc<TernaryExpr>(loc, cond, left, right);
    }

    Expr* parse_precedence(Precedence precedence) {
#define MEMBER_FN(object,ptrToMember)  ((object).*(ptrToMember))
        advance();
        TokenType prefix_type = previous().type;
        PrefixParseFn prefix_rule = get_rule(prefix_type).prefix_fn;
        if (prefix_rule == nullptr) {
            return error_expr(get_current_token_loc(), "Expect expression.");
        }

        bool can_assign = precedence <= Precedence::Assignment;
        Expr* expr = MEMBER_FN(*this, prefix_rule)(can_assign);

        while (precedence <= get_rule(current().type).precedence) {
            advance();
            InfixParseFn infix_rule = get_rule(previous().type).infix_fn;
            expr = MEMBER_FN(*this, infix_rule)(can_assign, expr);
        }

        if (can_assign && match(TokenType::Equal)) {
            return error_expr(get_current_token_loc(), "Invalid assignment target.");
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
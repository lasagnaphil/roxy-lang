#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/ast.hpp"

namespace rx {

struct ParseError {
    SourceLocation loc;
    const char* message;
};

class Parser {
public:
    Parser(Lexer& lexer, BumpAllocator& allocator);

    Program* parse();

    bool has_error() const { return m_has_error; }
    const ParseError& error() const { return m_error; }

private:
    Lexer& m_lexer;
    BumpAllocator& m_allocator;
    Token m_current;
    Token m_previous;
    bool m_has_error;
    ParseError m_error;

    // Token operations
    void advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    Token consume(TokenKind kind, const char* message);
    bool is_at_end() const;

    // Error handling
    void report_error(const char* message);
    void report_error_at(const Token& token, const char* message);

    // Allocation helpers
    template <typename T>
    T* alloc() {
        return m_allocator.emplace<T>();
    }

    template <typename T>
    Span<T> alloc_span(const Vector<T>& vec);

    // Expression parsing (Pratt parser)
    Expr* expression();
    Expr* parse_precedence(u8 min_prec);
    Expr* unary();
    Expr* postfix(Expr* left);
    Expr* primary();

    // Helper to create binary expression node
    Expr* make_binary(Expr* left, BinaryOp op, Expr* right, SourceLocation loc);

    // Call expression helpers
    Expr* finish_call(Expr* callee);
    Expr* finish_index(Expr* object);

    // Statement parsing
    Stmt* statement();
    Stmt* block_statement();
    Stmt* if_statement();
    Stmt* while_statement();
    Stmt* for_statement();
    Stmt* return_statement();
    Stmt* break_statement();
    Stmt* continue_statement();
    Stmt* delete_statement();
    Stmt* when_statement();
    Stmt* throw_statement();
    Stmt* try_statement();
    Stmt* yield_statement();
    Stmt* expression_statement();

    // Declaration parsing
    Decl* declaration();
    Decl* var_declaration(bool is_pub);
    Decl* fun_declaration(bool is_pub, bool is_native);
    Decl* method_declaration(bool is_pub, bool is_native,
                              Token struct_token, Span<TypeParam> type_params);
    Decl* constructor_declaration(bool is_pub);
    Decl* destructor_declaration(bool is_pub);
    Decl* struct_declaration(bool is_pub);
    Decl* enum_declaration(bool is_pub);
    Decl* trait_declaration(bool is_pub);
    Decl* import_declaration();

    // Helper for parsing constructor/destructor (they share the same structure)
    struct CtorDtorParsed {
        StringView struct_name;
        StringView name;
        Vector<Param> params;
        Stmt* body;
    };
    bool parse_ctor_dtor_common(const char* kind_name, CtorDtorParsed& out);

    // Type parsing
    TypeExpr* type_expression();

    // Generic type parameter/argument parsing
    Span<TypeParam> parse_type_params();     // <T, U> in declarations
    Span<TypeExpr*> parse_type_args();       // <i32, string> in type annotations
    Span<TypeExpr*> try_parse_generic_args(); // Trial parse <types> in expression position

    // Closing angle bracket that handles >> splitting for nested generics
    bool consume_closing_angle();

    // Parser state save/restore for trial parsing
    struct SavedState {
        Token current;
        Token previous;
        Lexer::SavedPosition lexer_pos;
        bool has_error;
    };
    SavedState save_state();
    void restore_state(const SavedState& state);

    // Helper to parse parameter list
    Vector<Param> parse_parameters();

    // Helper to parse when field declaration (tagged union)
    WhenFieldDecl parse_when_field_decl();

    // Helper to process string literal (strip quotes, handle escapes)
    StringView process_string_literal(const Token& token);

    // Helper to process f-string part (strip delimiters, handle escapes including \{ \})
    StringView process_fstring_part(const Token& token);

    // Helper to parse dotted module path (e.g., "math.vec2")
    StringView parse_module_path();
};

}

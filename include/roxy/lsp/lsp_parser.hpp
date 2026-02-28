#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/string.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/lsp/syntax_tree.hpp"

namespace rx {

class LspParser {
public:
    LspParser(Lexer& lexer, BumpAllocator& allocator);

    SyntaxTree parse();

private:
    Lexer& m_lexer;
    BumpAllocator& m_allocator;
    Token m_current;
    Token m_previous;
    Vector<ParseDiagnostic> m_diagnostics;

    // Source info (cached from lexer)
    const char* m_source;
    u32 m_source_length;

    // Token operations
    void advance();
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    bool is_at_end() const;

    // Error-recovering consume: returns actual or synthetic token
    Token consume_or_synthetic(TokenKind expected, const char* message);

    // Error recovery
    void synchronize_to_statement_boundary();
    void skip_to_closing_bracket(TokenKind open, TokenKind close);
    SyntaxNode* make_error_node(const char* message);
    void add_diagnostic(TextRange range, const char* message);

    // Node construction
    struct NodeBuilder {
        SyntaxKind kind;
        u32 start_offset;
        Vector<SyntaxNode*> children;
    };
    NodeBuilder begin_node(SyntaxKind kind);
    SyntaxNode* finish_node(NodeBuilder& builder);
    SyntaxNode* make_token_node(const Token& token);

    // Allocation helpers
    template <typename T>
    Span<T> alloc_span(const Vector<T>& vec);

    // Modifier tokens captured before dispatching to sub-parsers
    struct DeclModifiers {
        Token pub_token;
        Token native_token;
        bool has_pub = false;
        bool has_native = false;
    };

    // Grammar productions — declarations
    SyntaxNode* parse_program();
    SyntaxNode* parse_declaration();
    SyntaxNode* parse_var_decl(const DeclModifiers& mods);
    SyntaxNode* parse_fun_decl(const DeclModifiers& mods);
    SyntaxNode* parse_method_decl(NodeBuilder& builder, const DeclModifiers& mods);
    SyntaxNode* parse_constructor_decl(const DeclModifiers& mods);
    SyntaxNode* parse_destructor_decl(const DeclModifiers& mods);
    SyntaxNode* parse_struct_decl(const DeclModifiers& mods);
    SyntaxNode* parse_enum_decl(const DeclModifiers& mods);
    SyntaxNode* parse_trait_decl(const DeclModifiers& mods);
    SyntaxNode* parse_import_decl();
    SyntaxNode* parse_field_decl(const DeclModifiers& mods);
    SyntaxNode* parse_when_field_decl();

    // Helper: insert pub/native modifier tokens as leading children
    void insert_modifier_children(NodeBuilder& builder, const DeclModifiers& mods);

    // Grammar productions — statements
    SyntaxNode* parse_statement();
    SyntaxNode* parse_block_stmt();
    SyntaxNode* parse_if_stmt();
    SyntaxNode* parse_while_stmt();
    SyntaxNode* parse_for_stmt();
    SyntaxNode* parse_return_stmt();
    SyntaxNode* parse_break_stmt();
    SyntaxNode* parse_continue_stmt();
    SyntaxNode* parse_delete_stmt();
    SyntaxNode* parse_when_stmt();
    SyntaxNode* parse_throw_stmt();
    SyntaxNode* parse_try_stmt();
    SyntaxNode* parse_expr_stmt();

    // Grammar productions — expressions
    SyntaxNode* parse_expression();
    SyntaxNode* parse_precedence(u8 min_prec);
    SyntaxNode* parse_unary();
    SyntaxNode* parse_postfix(SyntaxNode* left);
    SyntaxNode* parse_primary();
    SyntaxNode* parse_call_args(NodeBuilder& builder);
    SyntaxNode* parse_fstring();

    // Type parsing
    SyntaxNode* parse_type_expr();
    SyntaxNode* parse_type_params();
    SyntaxNode* parse_type_args();
    bool try_parse_generic_args(NodeBuilder& parent_builder);

    // Closing angle bracket that handles >> splitting for nested generics
    bool consume_closing_angle(NodeBuilder& builder);

    // Parameter list
    SyntaxNode* parse_param_list();
    SyntaxNode* parse_param();

    // Parser state save/restore for trial parsing
    struct SavedState {
        Token current;
        Token previous;
        Lexer::SavedPosition lexer_pos;
        u32 diagnostic_count;
    };
    SavedState save_state();
    void restore_state(const SavedState& state);

    // Helpers
    bool is_statement_start(TokenKind kind) const;
    SyntaxNode* parse_module_path(NodeBuilder& builder);
};

} // namespace rx

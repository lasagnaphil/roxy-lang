#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/lsp/syntax_tree.hpp"

namespace rx {

class CstLowering {
public:
    CstLowering(BumpAllocator& allocator);

    // Lower a full program
    Program* lower(SyntaxNode* root);

    // Lower a single function-like declaration (for targeted analysis)
    Decl* lower_decl(SyntaxNode* node);

private:
    BumpAllocator& m_allocator;

    // Per-kind lowering — declarations
    Decl* lower_top_level_node(SyntaxNode* node);
    Decl* lower_var_decl(SyntaxNode* node);
    Decl* lower_fun_decl(SyntaxNode* node);
    Decl* lower_method_decl(SyntaxNode* node);
    Decl* lower_constructor_decl(SyntaxNode* node);
    Decl* lower_destructor_decl(SyntaxNode* node);
    Decl* lower_struct_decl(SyntaxNode* node);
    Decl* lower_enum_decl(SyntaxNode* node);
    Decl* lower_trait_decl(SyntaxNode* node);
    Decl* lower_import_decl(SyntaxNode* node);

    // Statements
    Stmt* lower_stmt(SyntaxNode* node);
    Stmt* lower_block_stmt(SyntaxNode* node);
    Stmt* lower_if_stmt(SyntaxNode* node);
    Stmt* lower_while_stmt(SyntaxNode* node);
    Stmt* lower_for_stmt(SyntaxNode* node);
    Stmt* lower_return_stmt(SyntaxNode* node);
    Stmt* lower_when_stmt(SyntaxNode* node);
    Stmt* lower_try_stmt(SyntaxNode* node);
    Stmt* lower_throw_stmt(SyntaxNode* node);
    Stmt* lower_delete_stmt(SyntaxNode* node);
    Stmt* lower_yield_stmt(SyntaxNode* node);
    Stmt* lower_expr_stmt(SyntaxNode* node);

    // Expressions
    Expr* lower_expr(SyntaxNode* node);
    Expr* lower_literal_expr(SyntaxNode* node);
    Expr* lower_identifier_expr(SyntaxNode* node);
    Expr* lower_unary_expr(SyntaxNode* node);
    Expr* lower_binary_expr(SyntaxNode* node);
    Expr* lower_ternary_expr(SyntaxNode* node);
    Expr* lower_call_expr(SyntaxNode* node);
    Expr* lower_index_expr(SyntaxNode* node);
    Expr* lower_get_expr(SyntaxNode* node);
    Expr* lower_static_get_expr(SyntaxNode* node);
    Expr* lower_assign_expr(SyntaxNode* node);
    Expr* lower_grouping_expr(SyntaxNode* node);
    Expr* lower_self_expr(SyntaxNode* node);
    Expr* lower_super_expr(SyntaxNode* node);
    Expr* lower_struct_literal_expr(SyntaxNode* node);
    Expr* lower_string_interp_expr(SyntaxNode* node);

    // Type expressions and parameters
    TypeExpr* lower_type_expr(SyntaxNode* node);
    Span<Param> lower_param_list(SyntaxNode* node);
    Span<TypeParam> lower_type_param_list(SyntaxNode* node);

    // Helpers
    template<typename T> T* alloc();
    SyntaxNode* find_child(SyntaxNode* node, SyntaxKind kind);
    SyntaxNode* find_child_after(SyntaxNode* node, SyntaxKind kind, u32 start);
    bool has_child(SyntaxNode* node, SyntaxKind kind);
    SourceLocation make_loc(SyntaxNode* node);
    SourceLocation make_loc(TextRange range);
};

} // namespace rx

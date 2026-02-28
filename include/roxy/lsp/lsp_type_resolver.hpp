#pragma once

#include "roxy/core/string.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/compiler/ast.hpp"
#include "roxy/lsp/syntax_tree.hpp"
#include "roxy/lsp/global_index.hpp"

namespace rx {

class LspTypeResolver {
public:
    LspTypeResolver(const GlobalIndex& index);

    // Analyze a lowered function/method to populate variable scope
    void analyze_function(Decl* fun_decl);

    // Resolve the type name of a CST expression using the built scope
    // Returns empty string if unresolvable
    String resolve_cst_expr_type(SyntaxNode* expr_node) const;

private:
    const GlobalIndex& m_index;

    // Variable scope: name -> type_name (flat; latest declaration wins)
    tsl::robin_map<String, String> m_var_types;

    // Enclosing struct name (for self resolution in methods)
    String m_self_type;

    // AST-based analysis (builds m_var_types)
    void analyze_block(Stmt* block);
    void analyze_stmt(Stmt* stmt);
    void analyze_decl(Decl* decl);
    String resolve_ast_expr_type(Expr* expr) const;

    // CST-based expression resolution (uses m_var_types + GlobalIndex)
    String resolve_field_type_in_hierarchy(StringView struct_name, StringView field_name) const;
};

} // namespace rx

#pragma once

#include "roxy/core/string.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/compiler/ast.hpp"
#include "roxy/lsp/syntax_tree.hpp"
#include "roxy/lsp/global_index.hpp"
#include "roxy/lsp/protocol.hpp"

namespace rx {

struct SemanticDiagnostic {
    TextRange range;
    DiagnosticSeverity severity;
    String message;
};

class LspTypeResolver {
public:
    LspTypeResolver(const GlobalIndex& index);

    // Analyze a lowered function/method to populate variable scope
    void analyze_function(Decl* fun_decl);

    // Analyze with diagnostic collection
    void analyze_function_with_diagnostics(Decl* fun_decl);

    // Resolve the type name of a CST expression using the built scope
    // Returns empty string if unresolvable
    String resolve_cst_expr_type(SyntaxNode* expr_node) const;

    // Accessors for completion support
    const tsl::robin_map<String, String>& var_types() const { return m_var_types; }
    const String& self_type() const { return m_self_type; }

    // Get collected diagnostics
    const Vector<SemanticDiagnostic>& diagnostics() const { return m_diagnostics; }

private:
    const GlobalIndex& m_index;

    // Variable scope: name -> type_name (flat; latest declaration wins)
    tsl::robin_map<String, String> m_var_types;

    // Enclosing struct name (for self resolution in methods)
    String m_self_type;

    // Collected semantic diagnostics
    Vector<SemanticDiagnostic> m_diagnostics;

    // AST-based analysis (builds m_var_types)
    void analyze_block(Stmt* block);
    void analyze_stmt(Stmt* stmt);
    void analyze_decl(Decl* decl);
    String resolve_ast_expr_type(Expr* expr) const;

    // Diagnostic checking (walks AST after analyze_function)
    void check_decl(Decl* decl);
    void check_stmt(Stmt* stmt);
    void check_expr(Expr* expr);
    void check_type_annotation(TypeExpr* type_expr);
    bool is_known_type(StringView name) const;
    void add_diagnostic(TextRange range, DiagnosticSeverity severity, String message);

    // Field/method existence checking in hierarchy
    bool has_field_in_hierarchy(StringView struct_name, StringView field_name) const;
    bool has_method_in_hierarchy(StringView struct_name, StringView method_name) const;

    // CST-based expression resolution (uses m_var_types + GlobalIndex)
    String resolve_field_type_in_hierarchy(StringView struct_name, StringView field_name) const;
};

} // namespace rx

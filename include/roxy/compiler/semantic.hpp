#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"

namespace rx {

// Forward declarations
class NativeRegistry;
class ModuleRegistry;

// Semantic error with location information
struct SemanticError {
    SourceLocation loc;
    const char* message;

    // For formatted messages, we store them in the allocator
    bool owns_message;
};

// Maximum number of errors to collect before stopping
constexpr u32 MAX_SEMANTIC_ERRORS = 20;

// SemanticAnalyzer performs type checking and symbol resolution
class SemanticAnalyzer {
public:
    // Constructor with external TypeCache and ModuleRegistry
    // Both must outlive the SemanticAnalyzer
    SemanticAnalyzer(BumpAllocator& allocator, TypeCache& types, ModuleRegistry& modules);

    // Analyze a program - returns true if no errors
    bool analyze(Program* program);

    // Error access
    bool has_errors() const { return !m_errors.empty(); }
    const Vector<SemanticError>& errors() const { return m_errors; }

    // Access type cache for external use
    TypeCache& types() { return m_types; }

    // Access symbol table for external use (e.g., IR builder needs it for imported functions)
    SymbolTable& symbols() { return m_symbols; }

private:
    // Error reporting
    void error(SourceLocation loc, const char* message);
    void error_fmt(SourceLocation loc, const char* fmt, ...);
    bool too_many_errors() const { return m_errors.size() >= MAX_SEMANTIC_ERRORS; }

    // Auto-import builtin module exports as prelude
    void import_builtin_prelude();

    // Multi-pass analysis
    void collect_type_declarations(Program* program);
    void resolve_type_members(Program* program);
    void analyze_function_bodies(Program* program);

    // Type resolution from AST TypeExpr
    Type* resolve_type_expr(TypeExpr* type_expr);

    // Declaration analysis
    void analyze_decl(Decl* decl);
    void analyze_var_decl(Decl* decl);
    void analyze_fun_decl(Decl* decl);
    void analyze_struct_decl(Decl* decl);
    void analyze_enum_decl(Decl* decl);
    void analyze_import_decl(Decl* decl);
    void analyze_constructor_decl(Decl* decl);
    void analyze_destructor_decl(Decl* decl);
    void analyze_method_decl(Decl* decl);
    void analyze_constructor_body(Decl* decl, Type* struct_type);
    void analyze_destructor_body(Decl* decl, Type* struct_type);
    void analyze_method_body(Decl* decl, Type* struct_type);

    // Statement analysis
    void analyze_stmt(Stmt* stmt);
    void analyze_expr_stmt(Stmt* stmt);
    void analyze_block_stmt(Stmt* stmt);
    void analyze_if_stmt(Stmt* stmt);
    void analyze_while_stmt(Stmt* stmt);
    void analyze_for_stmt(Stmt* stmt);
    void analyze_return_stmt(Stmt* stmt);
    void analyze_break_stmt(Stmt* stmt);
    void analyze_continue_stmt(Stmt* stmt);
    void analyze_delete_stmt(Stmt* stmt);

    // Expression analysis - returns the type of the expression
    Type* analyze_expr(Expr* expr);
    Type* analyze_literal_expr(Expr* expr);
    Type* analyze_identifier_expr(Expr* expr);
    Type* analyze_unary_expr(Expr* expr);
    Type* analyze_binary_expr(Expr* expr);
    Type* analyze_ternary_expr(Expr* expr);
    Type* analyze_call_expr(Expr* expr);
    Type* analyze_primitive_cast(Expr* expr, Type* target_type);
    Type* analyze_constructor_call(Expr* expr, Type* struct_type, StringView ctor_name, bool is_heap);

    // Cast checking helper
    bool can_cast(Type* source, Type* target);
    Type* analyze_index_expr(Expr* expr);
    Type* analyze_get_expr(Expr* expr);
    Type* analyze_static_get_expr(Expr* expr);
    Type* analyze_assign_expr(Expr* expr);
    Type* analyze_grouping_expr(Expr* expr);
    Type* analyze_this_expr(Expr* expr);
    Type* analyze_super_expr(Expr* expr);
    Type* analyze_struct_literal_expr(Expr* expr);

    // Type checking helpers
    bool check_assignable(Type* target, Type* source, SourceLocation loc);
    bool check_numeric(Type* type, SourceLocation loc);
    bool check_integer(Type* type, SourceLocation loc);
    bool check_boolean(Type* type, SourceLocation loc);
    Type* get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc);
    Type* get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc);

    // Type mismatch error helpers
    // Returns true if types match, false and reports error if they don't
    bool require_types_match(Type* left, Type* right, SourceLocation loc, const char* context);
    // Reports a type conversion error (source -> target)
    void error_cannot_convert(Type* source, Type* target, SourceLocation loc, const char* context);

    // Lvalue checking for assignment
    bool is_lvalue(Expr* expr) const;

    // Reference type conversion rules
    bool can_convert_ref(Type* from, Type* to) const;

    BumpAllocator& m_allocator;
    TypeCache& m_types;
    ModuleRegistry& m_modules;
    SymbolTable m_symbols;
    Vector<SemanticError> m_errors;
    Program* m_program;                   // Current program being analyzed

    // Named type lookup (structs/enums by name)
    tsl::robin_map<StringView, Type*, StringViewHash, StringViewEqual> m_named_types;
};

}

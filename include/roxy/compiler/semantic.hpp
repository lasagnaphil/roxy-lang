#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/compiler/generics.hpp"

namespace rx {

// Forward declarations
class NativeRegistry;
class ModuleRegistry;

// Result of generic type argument inference
struct InferredTypeArgs {
    bool success;
    Vector<Type*> type_args;  // indexed by type param position
};

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
    SemanticAnalyzer(BumpAllocator& allocator, TypeCache& types, ModuleRegistry& modules,
                     NativeRegistry* registry = nullptr);

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
    template<typename... Args>
    void error_fmt(SourceLocation loc, const char* fmt, const Args&... args) {
        if (too_many_errors()) return;

        char buffer[512];
        format_to(buffer, sizeof(buffer), fmt, args...);

        u32 len = static_cast<u32>(strlen(buffer));
        char* msg = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
        memcpy(msg, buffer, len + 1);

        m_errors.push_back({loc, msg, true});
    }
    bool too_many_errors() const { return m_errors.size() >= MAX_SEMANTIC_ERRORS; }

    // Auto-import builtin module exports as prelude
    void import_builtin_prelude();

    // Multi-pass analysis
    void collect_type_declarations(Program* program);
    void resolve_type_members(Program* program);
    void resolve_when_clauses(Span<WhenFieldDecl> when_decls,
                              Vector<FieldInfo>& fields,
                              Vector<WhenClauseInfo>& when_clauses,
                              u32& current_slot);
    void analyze_function_bodies(Program* program);

    // Type resolution from AST TypeExpr
    Type* resolve_type_expr(TypeExpr* type_expr);

    // Resolve fields of a generic struct instance (idempotent)
    void resolve_generic_struct_fields(GenericStructInstance* inst);

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
    void analyze_when_stmt(Stmt* stmt);

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

    // Call expression sub-helpers (extracted from analyze_call_expr)
    Type* analyze_generic_fun_call(Expr* expr, CallExpr& ce, StringView func_name);
    Type* analyze_list_constructor_call(Expr* expr, CallExpr& ce);
    Type* analyze_generic_struct_constructor_call(Expr* expr, CallExpr& ce, StringView func_name);
    Type* analyze_super_call(Expr* expr, CallExpr& ce);
    Type* analyze_builtin_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, const MethodInfo* mi);
    Type* analyze_struct_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, Type* base_type);
    Type* analyze_regular_fun_call(Expr* expr, CallExpr& ce);

    // Shared argument checking for method/function calls
    void check_call_args(Span<CallArg> args, Span<Type*> param_types,
                         Span<Param> params, SourceLocation loc);

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
    Type* analyze_string_interp_expr(Expr* expr);

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
    NativeRegistry* m_registry;
    SymbolTable m_symbols;
    Vector<SemanticError> m_errors;
    Program* m_program;                   // Current program being analyzed

    // Generics support
    GenericInstantiator m_generics;

    // Named type lookup (structs/enums by name)
    tsl::robin_map<StringView, Type*> m_named_types;

    // Trait support
    Type* m_printable_type = nullptr;  // Builtin Printable trait for f-string interpolation
    tsl::robin_map<StringView, Type*> m_trait_types;
    Vector<Decl*> m_synthetic_decls;  // Injected default method declarations

    // Generic type argument inference
    bool unify_type_expr(TypeExpr* pattern, Type* concrete,
                         Span<TypeParam> type_params, Vector<Type*>& bindings);
    InferredTypeArgs infer_type_args_from_call(Span<TypeParam> type_params,
                                                Span<Param> params,
                                                Span<CallArg> args,
                                                SourceLocation loc);
    InferredTypeArgs infer_type_args_from_fields(Span<TypeParam> type_params,
                                                  Span<FieldDecl> template_fields,
                                                  Span<FieldInit> literal_fields,
                                                  SourceLocation loc);

    // List method population from NativeRegistry
    void populate_list_methods(Type* list_type);
    NativeRegistry* get_builtin_registry();

    // Trait analysis helpers
    void analyze_trait_method_decl(Decl* decl, Type* trait_type);
    void validate_trait_implementations();
    void inject_default_method(Type* struct_type, Type* trait_type,
                               TraitMethodInfo& tmi, Span<Type*> trait_type_args);

    // Pending trait implementations (struct_name resolved to struct type + trait decl)
    struct PendingTraitImpl {
        Decl* decl;
        Type* struct_type;
        Type* trait_type;
        Span<Type*> trait_type_args;   // Resolved type args for generic traits
    };
    Vector<PendingTraitImpl> m_pending_trait_impls;

public:
    const Vector<Decl*>& synthetic_decls() const { return m_synthetic_decls; }

    // Access generics for IR builder
    GenericInstantiator& generics() { return m_generics; }
};

}

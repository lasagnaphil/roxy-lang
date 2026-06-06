#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/format.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/core/tsl/robin_set.h"

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
constexpr u32 MAX_LSP_SEMANTIC_ERRORS = 200;

// Move state for uniq ownership tracking
enum class MoveState : u8 {
    Live,         // Variable owns a valid value
    Moved,        // Ownership has been transferred (use is an error)
    MaybeValid,   // Conditionally moved (e.g., moved in one branch of if/else)
};

// SemanticAnalyzer performs type checking and symbol resolution
class SemanticAnalyzer {
public:
    // Constructor with external TypeEnv and ModuleRegistry
    // Both must outlive the SemanticAnalyzer
    SemanticAnalyzer(BumpAllocator& allocator, TypeEnv& type_env, ModuleRegistry& modules,
                     NativeRegistry* registry = nullptr);

    // Constructor with external SymbolTable (for persisting symbols across phases)
    SemanticAnalyzer(BumpAllocator& allocator, TypeEnv& type_env, ModuleRegistry& modules,
                     SymbolTable& external_symbols, NativeRegistry* registry = nullptr);

    // Analyze a program - returns true if no errors
    bool analyze(Program* program);

    // Sub-pass entry points for LSP use
    // Run declaration passes (0-2): imports, type collection, member resolution
    void run_declaration_passes(Program* program);

    // Run body analysis pass (3) on all functions in the program
    void run_body_analysis(Program* program);

    // Analyze a single function/method/constructor/destructor body
    // Uses the already-populated TypeEnv for type lookups
    void analyze_single_function(Decl* decl);

    // Set the program context (used by the post-pass below to know the
    // analyzer's module name without re-running body analysis).
    void set_program(Program* program);

    // Analyze pending generic-function instances whose template lives in this
    // analyzer's module. Used by the compiler's post-pass to handle
    // cross-module instantiations (e.g. module B uses module A's `identity<T>`):
    // each module's analyzer drains the instances it owns, ensuring the body
    // resolves against the right symbol table. Returns the number of
    // instances drained (the rest are re-queued for other modules).
    u32 analyze_owned_pending_fun_instances();

    // LSP mode: more tolerant of errors, handles null AST children gracefully
    void set_lsp_mode(bool enable);
    bool lsp_mode() const;

    // Error access
    bool has_errors() const { return !m_errors.empty(); }
    const Vector<SemanticError>& errors() const { return m_errors; }

    // Access type environment for external use
    TypeEnv& type_env() { return m_type_env; }

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
    bool too_many_errors() const;

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
    void analyze_member_body(Decl* decl, Type* struct_type,
                             Span<Param> params, Stmt* body, Type* return_type);
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
    void analyze_throw_stmt(Stmt* stmt);
    void analyze_try_stmt(Stmt* stmt);
    void analyze_yield_stmt(Stmt* stmt);

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
    Type* analyze_map_constructor_call(Expr* expr, CallExpr& ce);
    Type* analyze_generic_struct_constructor_call(Expr* expr, CallExpr& ce, StringView func_name);
    Type* analyze_super_call(Expr* expr, CallExpr& ce);
    Type* analyze_builtin_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, const MethodInfo* mi);
    Type* analyze_struct_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, Type* base_type);
    Type* analyze_type_param_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type,
                                          Type* type_param_type, const TraitMethodInfo* trait_method,
                                          Type* found_in_trait);
    Type* analyze_regular_fun_call(Expr* expr, CallExpr& ce);

    // Shared argument checking for method/function calls
    void check_call_args(Span<CallArg> args, Span<Type*> param_types,
                         Span<Param> params, SourceLocation loc);

    // Build a function type for a method call: (ref<self_type>, method_params...) -> return_type
    Type* build_method_function_type(Type* self_type, const MethodInfo* method_info);

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
    Type* analyze_lambda_expr(Expr* expr);

    // Recursively populates implicit ref-self captures in lambda contexts
    // 0..target_idx (inclusive). Outermost reads `self` directly via ExprThis;
    // every inner one reads via ExprGet(__env, __self) on the next-outer env.
    // Idempotent (skips contexts that already have has_self_capture set).
    void ensure_self_captured_through(u32 target_idx, Type* struct_type,
                                      SourceLocation loc);

    // Integer literal coercion: concretizes IntLiteral expressions to a target integer type
    void coerce_int_literal(Expr* expr, Type* target);

    // Generic-template-ref coercion: when an identifier was deferred by
    // analyze_identifier_expr (it named a generic function template), and the
    // surrounding context expects a concrete function type, infer the type
    // arguments from `expected`, instantiate the template, stash the
    // monomorphized name on the identifier, and overwrite resolved_type.
    // Returns true on successful coercion (or no-op when no coercion needed),
    // false if the expression is a template ref but inference failed.
    bool coerce_generic_template_ref(Expr* expr, Type* expected);

    // Explicit-syntax generic ref: `identity<i32>` parsed in value position.
    // The parser attached `generic_args` to the IdentifierExpr; this helper
    // instantiates the template with those types (no inference), stashes the
    // monomorphized name, and returns the resulting concrete function type.
    Type* resolve_explicit_generic_template_ref(Expr* expr);

    // Type checking helpers
    bool is_assignable(Type* target, Type* source) const;
    bool check_assignable(Type* target, Type* source, SourceLocation loc);
    bool check_numeric(Type* type, SourceLocation loc);
    bool check_integer(Type* type, SourceLocation loc);
    bool check_boolean(Type* type, SourceLocation loc);
    Type* get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc);
    Type* get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc);

    // Unified operator dispatch helpers (work for both primitives and structs)
    Type* try_resolve_binary_op(BinaryOp op, Type* left, Type* right);
    Type* try_resolve_unary_op(UnaryOp op, Type* operand);

    // Register built-in operator methods for primitive types
    void register_primitive_operator_methods();

    // Convert a Type* to a null-terminated string for use in error messages
    String type_string(Type* type);

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
    TypeEnv& m_type_env;
    TypeCache& m_types;  // Cached ref to m_type_env.types() to minimize churn
    ModuleRegistry& m_modules;
    NativeRegistry* m_registry;
    UniquePtr<SymbolTable> m_owned_symbols;  // null when using external symbols
    SymbolTable& m_symbols;
    Vector<SemanticError> m_errors;
    Program* m_program;                   // Current program being analyzed
    bool m_lsp_mode = false;              // When true, more tolerant of errors

    // Coroutine tracking (set while analyzing a function returning Coro<T>)
    bool m_in_coroutine = false;
    Type* m_coro_yield_type = nullptr;

    // Phase B: Active type parameter bounds (set when analyzing generic template bodies)
    // Maps type param index → resolved trait bounds
    Span<Span<TraitBound>> m_active_type_param_bounds;
    // The type params of the current generic template being checked
    Span<TypeParam> m_active_type_params;

    // Delete destructor tracking (throw is forbidden inside delete destructors)
    bool m_in_delete_destructor = false;

    // Finally depth tracking (for yield-in-finally validation)
    u32 m_in_finally_depth = 0;

    // Definite-termination analysis: true after analyzing a statement that
    // always transfers control out of the current join point (return/throw/
    // break/continue). Sticky through straight-line blocks; reset inside
    // loop bodies; consumed at if/when/try merge sites to pick the surviving
    // branch's move-state snapshot instead of producing MaybeValid.
    bool m_branch_terminates = false;

    Vector<Decl*> m_synthetic_decls;  // Injected default method declarations and lifted lambdas

    // Counter for unique lambda IDs (used to name synthesized env structs and call functions).
    u32 m_lambda_id_counter = 0;

    // Active lambda-body capture context. Pushed when entering analyze_lambda_expr,
    // popped on exit. analyze_identifier_expr inspects the topmost context to detect
    // captures (the symbol's defining scope sits past a ScopeKind::Lambda boundary).
    // For nested closures, multiple contexts are stacked (innermost on top).
    struct LambdaCaptureContext {
        Scope* boundary_scope;                       // the ScopeKind::Lambda for this lambda
        Type* env_struct_type;                       // the env struct (for cross-context __env typing)
        Vector<CaptureInfo> captures;                // ordered, env-field order
        tsl::robin_map<Symbol*, u32> by_symbol;      // dedup + index lookup
        // Self-capture state. Tracks whether `self` has been captured into this
        // lambda's env (and where in `captures`). When set by a [copy self] or
        // [weak self] entry pre-validation, body references to `self` route
        // through the existing entry rather than creating a duplicate.
        bool has_self_capture = false;
        u32 self_capture_index = 0;
    };
    Vector<LambdaCaptureContext*> m_lambda_contexts;

    // Cycle detection for direct value-type recursion in struct fields
    tsl::robin_set<Type*> m_resolving_structs;

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

    // List/Map/Coro/enum method population
    void populate_list_methods(Type* list_type);
    void populate_map_methods(Type* map_type);
    void populate_coro_methods(Type* coro_type);
    void populate_enum_methods(Type* enum_type);
    bool is_hashable_key_type(Type* type);
    NativeRegistry* get_builtin_registry();

    // Helpers for appending to bump-allocated Span lists on StructTypeInfo
    void append_method(StructTypeInfo& info, MethodInfo method);
    void append_constructor(StructTypeInfo& info, ConstructorInfo ctor);
    void append_destructor(StructTypeInfo& info, DestructorInfo dtor);

    // Phase B: Definition-site checking of generic template bodies
    void analyze_generic_template_body(Decl* decl);
    const TraitMethodInfo* lookup_type_param_method(Type* type_param_type, StringView method_name,
                                                      Type** found_in_trait);
    Type* substitute_trait_types(Type* type, Type* type_param, Type* found_in_trait);

    // Generic trait bound resolution and checking
    Span<TraitBound> resolve_type_param_bounds(Span<TypeExpr*> bound_exprs, SourceLocation loc);
    void resolve_generic_bounds();
    bool check_type_arg_bounds(StringView template_name, Span<Type*> type_args,
                               const ResolvedTypeParams* bounds, SourceLocation loc);

    // Trait analysis helpers
    Type* resolve_trait_type_expr(TypeExpr* type_expr, const TraitTypeInfo& trait_info);
    void analyze_trait_method_decl(Decl* decl, Type* trait_type);
    void validate_trait_implementations();
    void inject_default_method(Type* struct_type, Type* trait_type,
                               TraitMethodInfo& tmi, Span<Type*> trait_type_args);
    Type* resolve_trait_type(Type* abstract_type, Type* struct_type, Span<Type*> trait_type_args);

    // Pending trait implementations (struct_name resolved to struct type + trait decl)
    struct PendingTraitImpl {
        Decl* decl;
        Type* struct_type;
        Type* trait_type;
        Span<Type*> trait_type_args;   // Resolved type args for generic traits
    };
    Vector<PendingTraitImpl> m_pending_trait_impls;

    // Move-state tracking for noncopyable variables (per-function).
    // Keyed by Symbol* for correct handling of variable shadowing.
    tsl::robin_map<Symbol*, MoveState> m_move_states;

    // Save/restore move states for branching (if/else)
    using MoveStateSnapshot = tsl::robin_map<Symbol*, MoveState>;
    MoveStateSnapshot save_move_states() const { return m_move_states; }
    void restore_move_states(const MoveStateSnapshot& snapshot) { m_move_states = snapshot; }

    // Merge move states from two branches (e.g., if/else)
    void merge_move_states(const MoveStateSnapshot& then_states, const MoveStateSnapshot& else_states);

    // Branch-merge helpers shared by if/when/try (all operate on m_move_states).
    //
    // Two-way merge for if/else: keeps the surviving branch's snapshot when one
    // branch terminates, merges both when neither does, and sets
    // m_branch_terminates = then_terminates && else_terminates. A no-else `if`
    // calls this with else_states = pre_branch and else_terminates = false.
    void merge_two_branches(const MoveStateSnapshot& pre_branch,
                            const MoveStateSnapshot& then_states,
                            const MoveStateSnapshot& else_states,
                            bool then_terminates, bool else_terminates);

    // N-way merge for when/try: merges every non-terminating snapshot into
    // m_move_states. Returns true when every branch terminates (the join point
    // is unreachable), in which case m_move_states is restored to the first
    // snapshot. Does NOT touch m_branch_terminates — callers set it, since
    // when/try diverge afterwards (try has a finally clause).
    bool merge_branch_snapshots(const Vector<MoveStateSnapshot>& snapshots,
                                const Vector<bool>& terminates);

    // Check live uniq variables at scope exit for named-only destructors
    void check_scope_exit_uniq_destructors(const Scope* scope, SourceLocation loc);
    void check_all_scopes_uniq_destructors(SourceLocation loc, ScopeKind stop_kind);

    // Check if a uniq variable is in a moved state and report an error
    bool check_not_moved(StringView name, SourceLocation loc);

    // Check that an expression is not a field access with move-semantics type.
    // Returns false and reports error if it is (field-level moves are unsound).
    bool check_not_field_move(Expr* expr, SourceLocation loc);

    // Consume a noncopyable value: validates field-move legality and marks
    // the source identifier as moved. Call this at every point that transfers
    // ownership of a noncopyable value (var init, return, delete, call args,
    // assignment, struct literal fields).
    void consume_noncopyable(Expr* expr, SourceLocation loc);

    // Mark a uniq variable as moved
    void mark_moved(StringView name);

    // Mark a uniq variable as live (for reassignment)
    void mark_live(StringView name);

public:
    const Vector<Decl*>& synthetic_decls() const { return m_synthetic_decls; }

    // Access generics via type env
    GenericInstantiator& generics() { return m_type_env.generics(); }
};

}

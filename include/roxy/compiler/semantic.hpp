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
#include "roxy/compiler/error_reporter.hpp"
#include "roxy/compiler/type_checker.hpp"
#include "roxy/compiler/sema_context.hpp"
#include "roxy/compiler/function_context.hpp"
#include "roxy/compiler/lifetime_checker.hpp"
#include "roxy/compiler/trait_system.hpp"
#include "roxy/compiler/generic_call_resolver.hpp"
#include "roxy/core/tsl/robin_set.h"

namespace rx {

// Forward declarations
class NativeRegistry;
class ModuleRegistry;

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
    bool has_errors() const { return m_reporter.has_errors(); }
    const Vector<SemanticError>& errors() const { return m_reporter.errors(); }

    // Access type environment for external use
    TypeEnv& type_env() { return m_type_env; }

    // Access type cache for external use
    TypeCache& types() { return m_types; }

    // Access symbol table for external use (e.g., IR builder needs it for imported functions)
    SymbolTable& symbols() { return m_symbols; }

private:
    // Error reporting — thin forwarders to m_reporter so the many internal call
    // sites stay unchanged while the machinery lives in ErrorReporter.
    void error(SourceLocation loc, const char* message) { m_reporter.error(loc, message); }
    template<typename... Args>
    void error_fmt(SourceLocation loc, const char* fmt, const Args&... args) {
        m_reporter.error_fmt(loc, fmt, args...);
    }
    bool too_many_errors() const { return m_reporter.too_many_errors(); }

    // Auto-import builtin module exports as prelude
    void import_builtin_prelude();

    // Multi-pass analysis
    void collect_type_declarations(Program* program);
    void resolve_type_members(Program* program);
    // Per-declaration handlers dispatched by resolve_type_members.
    void resolve_struct_members(Decl* decl);
    void resolve_enum_members(Decl* decl);
    void register_fun_signature(Decl* decl);
    void resolve_global_var(Decl* decl);
    void resolve_constructor_member(Decl* decl);
    void resolve_destructor_member(Decl* decl);
    void resolve_method_member(Decl* decl);
    // Make sure `struct_type`'s members/layout are resolved before a dependent
    // struct embeds it by value — resolution recurses so declaration order
    // doesn't matter. Returns false (and reports the "infinite size" error at
    // `loc`) when `struct_type` is already being resolved higher up the
    // recursion, i.e. a genuine value-type cycle.
    bool ensure_struct_members_resolved(Type* struct_type, SourceLocation loc);
    // Trailing whole-program pass run after all members are resolved.
    void generate_synthetic_destructors(Program* program);
    void resolve_when_clauses(Span<WhenFieldDecl> when_decls,
                              Vector<FieldInfo>& fields,
                              Vector<WhenClauseInfo>& when_clauses,
                              u32& current_slot);
    void analyze_function_bodies(Program* program);

    // Type resolution from AST TypeExpr. Never returns null: a null TypeExpr
    // (LSP-recovered ASTs) and every resolution failure yield error_type, with
    // the error reported at the failure site. Callers that treat a *missing*
    // annotation as a signal (var-decl inference) branch on the TypeExpr.
    Type* resolve_type_expr(TypeExpr* type_expr);

    // Thunks installed into m_context so collaborators (TraitSystem,
    // GenericCallResolver) get TypeExpr resolution and walker re-entry
    // (generic inference analyzes call arguments; Phase B analyzes template
    // bodies) without a back-reference to the rest of the analyzer.
    static Type* resolve_type_expr_thunk(SemanticAnalyzer* analyzer, TypeExpr* type_expr) {
        return analyzer->resolve_type_expr(type_expr);
    }
    static Type* analyze_expr_thunk(SemanticAnalyzer* analyzer, Expr* expr) {
        return analyzer->analyze_expr(expr);
    }
    static void analyze_stmt_thunk(SemanticAnalyzer* analyzer, Stmt* stmt) {
        analyzer->analyze_stmt(stmt);
    }

    // Resolve fields of a generic struct instance (idempotent)
    void resolve_generic_struct_fields(GenericStructInstance* inst);

    // Drain one pending generic-fun instance: if its template belongs to this
    // module (or module ownership is indeterminate), analyze its body and mark
    // it analyzed; otherwise sideline it for the compiler's cross-module
    // post-pass (re-queueing it here would infinite-loop the worklist).
    // Returns true if analyzed here. Shared by analyze_owned_pending_fun_instances
    // and the worklist in analyze_function_bodies.
    bool drain_pending_fun_instance(GenericFunInstance* inst);

    // Pass 0 import handling
    void analyze_import_decl(Decl* decl);

    // Pass 2 signature registration: resolve param/return types and append
    // the ConstructorInfo/DestructorInfo/MethodInfo to the struct's tables.
    // No body analysis happens here (bodies are Pass 3).
    void register_constructor_signature(Decl* decl);
    void register_destructor_signature(Decl* decl);
    void register_method_signature(Decl* decl);

    // Shared building blocks for the register_*_signature functions above (and
    // the inline registration in resolve_generic_struct_fields):
    //   resolve_param_types  — resolve each param's TypeExpr into an arena span.
    //   resolve_member_struct — look up + kind-check the receiver struct;
    //     `noun` names the member kind for the "unknown struct" diagnostic.
    //   report_duplicate_member — reject a duplicate member `name` against
    //     `existing`, returning true (and reporting) on a clash. Works for
    //     ctors/dtors (empty name → "default") and methods (always named).
    Span<Type*> resolve_param_types(Span<Param> params);
    Type* resolve_member_struct(SourceLocation loc, StringView struct_name, const char* noun);
    template<typename InfoT>
    bool report_duplicate_member(SourceLocation loc, Span<InfoT> existing, StringView name,
                                 StringView struct_name, const char* noun);

    // Pass 3 body analysis (statement-level var decls are analyzed in-body).
    void analyze_var_decl(Decl* decl);
    void analyze_fun_body(Decl* decl);
    // Shared body analysis for constructors/destructors/methods; sets up the
    // per-function context (is_delete_destructor forbids throw in the body).
    void analyze_member_body(Decl* decl, Type* struct_type,
                             Span<Param> params, Stmt* body, Type* return_type,
                             bool is_delete_destructor = false);
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
    // Closure-capture path of analyze_identifier_expr: if `sym` resolves across
    // one or more enclosing lambda boundaries, record the capture(s), rewrite
    // `expr` in place to `__env.<name>`, set *out to the result type, and return
    // true. Returns false when no capture applies (caller handles normally).
    bool try_capture_identifier(Expr* expr, Symbol* sym, Type** out);
    // Walk from the current scope outward toward `stop_scope`, returning the
    // indices (innermost first) of every active lambda context whose boundary
    // Lambda scope is crossed on the way. Each active ScopeKind::Lambda scope
    // has exactly one matching context in m_lambda_contexts (both pushed in
    // synthesize_lambda_call_fn), so a crossed boundary always resolves to an
    // index. Shared by the identifier-capture path (stop at the symbol's
    // defining scope), the [move]-capture path (same), and self-capture
    // detection (stop at the struct scope; only non-emptiness matters).
    Vector<u32> collect_crossed_lambda_contexts(const Scope* stop_scope);
    Type* analyze_unary_expr(Expr* expr);
    Type* analyze_binary_expr(Expr* expr);
    Type* analyze_ternary_expr(Expr* expr);
    Type* analyze_call_expr(Expr* expr);
    Type* analyze_primitive_cast(Expr* expr, Type* target_type);
    Type* analyze_constructor_call(Expr* expr, Type* struct_type, StringView ctor_name, bool is_heap);

    // Call expression sub-helpers (extracted from analyze_call_expr).
    // Generic function calls (explicit and inferred type args) live on
    // m_generic_calls (see generic_call_resolver.hpp).
    Type* analyze_list_constructor_call(Expr* expr, CallExpr& ce);
    Type* analyze_map_constructor_call(Expr* expr, CallExpr& ce);
    Type* analyze_generic_struct_constructor_call(Expr* expr, CallExpr& ce, StringView func_name);
    Type* analyze_super_call(Expr* expr, CallExpr& ce);
    Type* analyze_builtin_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, const MethodInfo* mi);
    Type* analyze_struct_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type, Type* base_type);
    Type* analyze_regular_fun_call(Expr* expr, CallExpr& ce);

    // Shared argument checking for method/function calls
    void check_call_args(Span<CallArg> args, Span<Type*> param_types,
                         Span<Param> params, SourceLocation loc);

    // The per-argument type check shared by analyze_super_call's three arms
    // (default ctor / named ctor / method): analyze each arg, check
    // assignability against the parent param type, and coerce int literals.
    // Arity is checked by the caller — its diagnostic and bail-out result type
    // differ per arm. Simpler than check_call_args on purpose: super calls
    // carry no out/inout modifiers or move semantics.
    void check_super_call_arg_types(CallExpr& ce, Span<Type*> param_types);

    // Build a function type for a method call: (ref<self_type>, method_params...) -> return_type
    Type* build_method_function_type(Type* self_type, const MethodInfo* method_info);

    Type* analyze_index_expr(Expr* expr);
    Type* analyze_get_expr(Expr* expr);
    Type* analyze_static_get_expr(Expr* expr);
    Type* analyze_assign_expr(Expr* expr);
    Type* analyze_grouping_expr(Expr* expr);
    Type* analyze_this_expr(Expr* expr);
    Type* analyze_super_expr(Expr* expr);
    Type* analyze_struct_literal_expr(Expr* expr);
    // Phases of analyze_struct_literal_expr: resolve the (possibly generic)
    // struct type (returns error_type on failure), then type-check each field
    // initializer and report any missing required fields.
    Type* resolve_struct_literal_type(Expr* expr, StructLiteralExpr& sl);
    void check_struct_literal_fields(Expr* expr, StructLiteralExpr& sl, Type* type);
    Type* analyze_string_interp_expr(Expr* expr);
    Type* analyze_lambda_expr(Expr* expr);
    struct LambdaCaptureContext;  // defined below; used by the phase helpers
    // Phases of analyze_lambda_expr, sharing the per-lambda LambdaCaptureContext.
    // validate: pre-validate [move]/[copy self]/[weak self] entries and collect
    //   captures into `context` (false on error). synthesize: build the lifted
    //   call FunDecl and analyze its body. backfill: lay out the env struct.
    bool validate_lambda_captures(LambdaExpr& le, LambdaCaptureContext& context);
    Decl* synthesize_lambda_call_fn(Expr* expr, LambdaExpr& le,
                                    StringView fun_name, StringView env_name,
                                    Type* ret_type, LambdaCaptureContext& context);
    void backfill_lambda_env(Type* env_type, const LambdaCaptureContext& context);

    // Builders for synthetic AST nodes (used by lambda capture lowering and
    // self-capture rewriting). Each bump-allocates an Expr and fills in the
    // tagged-union member plus loc/resolved_type.
    Expr* make_identifier_expr(StringView name, Type* type, SourceLocation loc);
    Expr* make_get_expr(Expr* object, StringView name, Type* type, SourceLocation loc);
    Expr* make_this_expr(Type* type, SourceLocation loc);

    // Recursively populates implicit ref-self captures in lambda contexts
    // 0..target_idx (inclusive). Outermost reads `self` directly via ExprThis;
    // every inner one reads via ExprGet(__env, __self) on the next-outer env.
    // Idempotent (skips contexts that already have has_self_capture set).
    void ensure_self_captured_through(u32 target_idx, Type* struct_type,
                                      SourceLocation loc);

    // Operator result-type helpers (these dispatch through trait bounds, so they
    // stay on the analyzer; the pure relation/coercion checks live in TypeChecker)
    Type* get_binary_result_type(BinaryOp op, Type* left, Type* right, SourceLocation loc);
    Type* get_unary_result_type(UnaryOp op, Type* operand, SourceLocation loc);

    // Unified operator dispatch helpers (work for both primitives and structs)
    Type* try_resolve_binary_op(BinaryOp op, Type* left, Type* right);
    Type* try_resolve_unary_op(UnaryOp op, Type* operand);

    // Lvalue checking for assignment
    bool is_lvalue(Expr* expr) const;

    BumpAllocator& m_allocator;
    TypeEnv& m_type_env;
    TypeCache& m_types;  // Cached ref to m_type_env.types() to minimize churn
    ModuleRegistry& m_modules;
    NativeRegistry* m_registry;
    UniquePtr<SymbolTable> m_owned_symbols;  // null when using external symbols
    SymbolTable& m_symbols;
    ErrorReporter m_reporter;
    TypeChecker m_checker;
    // Shared collaborator context: the analyzer state bundle (allocator,
    // type_env, types, modules, symbols, reporter, checker) plus the
    // resolve_type_expr thunk, passed by reference to each extracted
    // collaborator below (see sema_context.hpp).
    SemaContext m_context;
    // Per-function lifetime analysis (move states, definite termination,
    // scope-exit destructor checks) — driven by the statement walkers here,
    // state and rules owned by the checker (see lifetime_checker.hpp).
    LifetimeChecker m_lifetimes;
    Vector<Decl*> m_synthetic_decls;  // Injected default method declarations and lifted lambdas
    // Trait machinery: builtin trait registration, trait declarations, impl
    // grouping/validation, default-method injection (see trait_system.hpp).
    TraitSystem m_traits;
    // Per-function context slots (coroutine / delete-destructor / finally
    // depth) consulted by the statement walkers; pushed and popped as one
    // unit (together with the lifetime state) by FunctionContextScope at
    // every body-analysis entry point (see function_context.hpp).
    FunctionContext m_function_context;
    // Generic-call machinery: type-arg unification/inference, generic function
    // calls, template refs in value position, trait bounds, and Phase B
    // template-body checking incl. the active-type-param state (see
    // generic_call_resolver.hpp).
    GenericCallResolver m_generic_calls;
    Program* m_program;                   // Current program being analyzed

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

    // List/Map/Coro/enum method population
    void populate_list_methods(Type* list_type);
    void populate_map_methods(Type* map_type);
    void populate_coro_methods(Type* coro_type);
    void populate_enum_methods(Type* enum_type);
    // Shared by populate_list_methods/populate_map_methods: instantiate the
    // container's native method table + alloc/copy native names from the
    // registry, writing into the caller's *_info fields (idempotent).
    void populate_container_methods(const char* registry_name, Span<Type*> type_args,
                                    Span<MethodInfo>& out_methods,
                                    StringView& out_alloc_name, StringView& out_copy_name);
    bool is_hashable_key_type(Type* type);
    NativeRegistry* get_builtin_registry();

public:
    const Vector<Decl*>& synthetic_decls() const { return m_synthetic_decls; }

    // Access generics via type env
    GenericInstantiator& generics() { return m_type_env.generics(); }
};

}

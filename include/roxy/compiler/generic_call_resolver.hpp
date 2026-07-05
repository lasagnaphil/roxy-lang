#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/compiler/error_reporter.hpp"
#include "roxy/compiler/type_checker.hpp"
#include "roxy/compiler/sema_context.hpp"
#include "roxy/compiler/lifetime_checker.hpp"
#include "roxy/compiler/function_context.hpp"

namespace rx {

// Result of generic type argument inference
struct InferredTypeArgs {
    bool success;
    Vector<Type*> type_args;  // indexed by type param position
};

// GenericCallResolver owns the generic-call machinery the semantic analyzer
// drives: type-argument unification/inference, both generic function call
// paths (explicit type args and inferred), generic-template refs in value
// position, trait-bound resolution and checking (Phase A, instantiation
// site), and Phase B definition-site checking of bounded template bodies —
// including the active-type-param state that the analyzer's type resolution
// and operator dispatch consult while a template body is being checked.
//
// Collaborators come in through the shared SemaContext (same pattern as
// LifetimeChecker/TraitSystem — no back-reference to the analyzer). Inference
// analyzes call arguments and Phase B walks template bodies, so this class
// re-enters the walkers through SemaContext::analyze_expr / analyze_stmt.
class GenericCallResolver {
public:
    GenericCallResolver(SemaContext& context, LifetimeChecker& lifetimes,
                        FunctionContext& function_context)
        : m_context(context)
        , m_allocator(context.allocator)
        , m_type_env(context.type_env)
        , m_types(context.types)
        , m_symbols(context.symbols)
        , m_reporter(context.reporter)
        , m_checker(context.checker)
        , m_lifetimes(lifetimes)
        , m_function_context(function_context) {}

    // ===== Pass 1.9: trait bounds on generic type parameters =====

    // Resolve every registered generic template's per-type-param trait bounds
    // into the GenericInstantiator's side tables.
    void resolve_generic_bounds();

    // Check concrete type args against a template's resolved bounds (Phase A,
    // instantiation-site). Reports an error per unsatisfied bound.
    bool check_type_arg_bounds(StringView template_name, Span<Type*> type_args,
                               const ResolvedTypeParams* bounds, SourceLocation loc);

    // ===== Generic function calls (Pass 3) =====

    // Generic function call with explicit type args: name<T>(args).
    Type* analyze_generic_fun_call(Expr* expr, CallExpr& ce, StringView func_name);
    // Generic function call without type args: infers them from the arguments.
    Type* analyze_generic_fun_call_inferred(Expr* expr, CallExpr& ce, StringView func_name);

    // ===== Generic-template refs in value position =====

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

    // ===== Struct-literal inference (used by analyze_struct_literal_expr) =====

    InferredTypeArgs infer_type_args_from_fields(Span<TypeParam> type_params,
                                                 Span<FieldDecl> template_fields,
                                                 Span<FieldInit> literal_fields,
                                                 SourceLocation loc);

    // ===== Phase B: definition-site checking of generic template bodies =====

    void analyze_generic_template_body(Decl* decl);

    // The accessors below serve the analyzer's type-param-aware paths (type
    // resolution, operator dispatch, method calls/field access on a type
    // param) while a bounded template body is active.

    bool has_active_bounds() const { return m_active_type_param_bounds.size() > 0; }

    // Resolve `name` against the active template's type parameters; returns
    // the interned TypeParam type, or null when no active param matches.
    Type* resolve_active_type_param(StringView name);

    // Find `method_name` among the active trait bounds of a type parameter
    // (searching each bound trait and its parents). Sets *found_in_trait to
    // the defining trait.
    const TraitMethodInfo* lookup_type_param_method(Type* type_param_type, StringView method_name,
                                                    Type** found_in_trait);

    // Substitute Self/trait-type-params in a trait-method type against the
    // active bounds of `type_param` (the receiver's type parameter).
    Type* substitute_trait_types(Type* type, Type* type_param, Type* found_in_trait);

    // Type-check a method call on a type-param receiver against the trait
    // method found via lookup_type_param_method.
    Type* analyze_type_param_method_call(Expr* expr, CallExpr& ce, GetExpr& ge, Type* obj_type,
                                         Type* type_param_type, const TraitMethodInfo* trait_method,
                                         Type* found_in_trait);

private:
    // Unify a template parameter's TypeExpr pattern against a concrete type,
    // binding type params by position into `bindings` (null = unbound).
    bool unify_type_expr(TypeExpr* pattern, Type* concrete,
                         Span<TypeParam> type_params, Vector<Type*>& bindings);

    InferredTypeArgs infer_type_args_from_call(Span<TypeParam> type_params,
                                               Span<Param> params,
                                               Span<CallArg> args,
                                               SourceLocation loc);

    // Shared tail for both generic-fun-call paths: validates argument count
    // against the instantiated function, type-checks each argument, resolves the
    // return type, and records the callee's concrete function type. When
    // args_pre_analyzed is true (inference path), arguments were already
    // analyzed during inference, so their resolved_type is read instead of
    // re-analyzing.
    Type* check_instantiated_generic_call(Expr* expr, CallExpr& ce, StringView func_name,
                                          GenericFunInstance* inst, bool args_pre_analyzed);

    Span<TraitBound> resolve_type_param_bounds(Span<TypeExpr*> bound_exprs, SourceLocation loc);
    // Resolve one template's per-type-param trait bounds; returns false (no
    // bounds) when no type param is bounded. Shared by both arms of
    // resolve_generic_bounds (generic functions and generic structs).
    bool resolve_template_bounds(Span<TypeParam> type_params, ResolvedTypeParams& out);

    SemaContext& m_context;  // resolve_type_expr / analyze_expr / analyze_stmt services
    BumpAllocator& m_allocator;
    TypeEnv& m_type_env;
    TypeCache& m_types;
    SymbolTable& m_symbols;
    ErrorReporter& m_reporter;
    TypeChecker& m_checker;
    LifetimeChecker& m_lifetimes;  // arg-consume in calls; bundled into the Phase B body guard
    // The analyzer's per-function context: Phase B body checking pushes a
    // fresh one (via FunctionContextScope) like every other body entry point.
    FunctionContext& m_function_context;

    // Phase B: active type parameter bounds (set while analyzing a bounded
    // generic template body); maps type param index → resolved trait bounds.
    Span<Span<TraitBound>> m_active_type_param_bounds;
    // The type params of the current generic template being checked.
    Span<TypeParam> m_active_type_params;
};

}

#pragma once

#include "roxy/compiler/parse/ast.hpp"
#include "roxy/compiler/sema/function_context.hpp"
#include "roxy/compiler/sema/lifetime_checker.hpp"
#include "roxy/compiler/sema/sema_context.hpp"
#include "roxy/compiler/support/error_reporter.hpp"
#include "roxy/compiler/types/symbol_table.hpp"
#include "roxy/compiler/types/type_env.hpp"
#include "roxy/compiler/types/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/token.hpp"

namespace rx {

// LambdaLifter owns the closure machinery the semantic analyzer drives:
// analyzing lambda expressions (capture pre-validation, lifting the body into
// a synthetic call function, env-struct layout backfill) and the capture
// rewrites applied to identifier / `self` references inside lambda bodies.
//
// Collaborators come in through the shared SemaContext (same pattern as
// LifetimeChecker/TraitSystem/GenericCallResolver — no back-reference to the
// analyzer). A lambda body is its own function, so this class re-enters the
// statement walker through SemaContext::analyze_stmt under a fresh
// FunctionContextScope, and resolves annotations through
// SemaContext::resolve_type_expr.
class LambdaLifter {
public:
    LambdaLifter(SemaContext& context, LifetimeChecker& lifetimes,
                 FunctionContext& function_context, Vector<Decl*>& synthetic_decls)
        : m_context(context), m_allocator(context.allocator), m_type_env(context.type_env),
          m_types(context.types), m_symbols(context.symbols), m_reporter(context.reporter),
          m_lifetimes(lifetimes), m_function_context(function_context),
          m_synthetic_decls(synthetic_decls) {}

    // Analyze a lambda expression: validate captures, synthesize the lifted
    // call FunDecl (pushed onto the analyzer's synthetic decls) and the env
    // struct, and return the user-facing `fun(...) -> R` function type.
    Type* analyze_lambda_expr(Expr* expr);

    // Closure-capture path of analyze_identifier_expr: if `sym` resolves across
    // one or more enclosing lambda boundaries, record the capture(s), rewrite
    // `expr` in place to `__env.<name>`, set *out to the result type, and return
    // true. Returns false when no capture applies (caller handles normally).
    bool try_capture_identifier(Expr* expr, Symbol* sym, Type** out);

    // Closure-capture path of analyze_this_expr: if the `self` reference sits
    // past a lambda boundary relative to the enclosing struct scope, capture
    // self through every crossed context, rewrite `expr` in place to
    // `__env.__self`, and return the capture's type (ref Self for implicit,
    // value Self for [copy], weak Self for [weak]). Returns null when no
    // capture applies (caller types `self` as a plain ref).
    Type* try_rewrite_self_capture(Expr* expr, Type* struct_type);

private:
    // Active lambda-body capture context. Pushed when entering analyze_lambda_expr,
    // popped on exit. try_capture_identifier inspects the topmost context to detect
    // captures (the symbol's defining scope sits past a ScopeKind::Lambda boundary).
    // For nested closures, multiple contexts are stacked (innermost on top).
    struct LambdaCaptureContext {
        Scope* boundary_scope;                  // the ScopeKind::Lambda for this lambda
        Type* env_struct_type;                  // the env struct (for cross-context __env typing)
        Vector<CaptureInfo> captures;           // ordered, env-field order
        tsl::robin_map<Symbol*, u32> by_symbol; // dedup + index lookup
        // Self-capture state. Tracks whether `self` has been captured into this
        // lambda's env (and where in `captures`). When set by a [copy self] or
        // [weak self] entry pre-validation, body references to `self` route
        // through the existing entry rather than creating a duplicate.
        bool has_self_capture = false;
        u32 self_capture_index = 0;
    };

    // Walk from the current scope outward toward `stop_scope`, returning the
    // indices (innermost first) of every active lambda context whose boundary
    // Lambda scope is crossed on the way. Each active ScopeKind::Lambda scope
    // has exactly one matching context in m_lambda_contexts (both pushed in
    // synthesize_lambda_call_fn), so a crossed boundary always resolves to an
    // index. Shared by the identifier-capture path (stop at the symbol's
    // defining scope), the [move]-capture path (same), and self-capture
    // detection (stop at the struct scope; only non-emptiness matters).
    Vector<u32> collect_crossed_lambda_contexts(const Scope* stop_scope);

    // Phases of analyze_lambda_expr, sharing the per-lambda LambdaCaptureContext.
    // validate: pre-validate [move]/[copy self]/[weak self] entries and collect
    //   captures into `context` (false on error). synthesize: build the lifted
    //   call FunDecl and analyze its body. backfill: lay out the env struct.
    bool validate_lambda_captures(LambdaExpr& le, LambdaCaptureContext& context);
    Decl* synthesize_lambda_call_fn(Expr* expr, LambdaExpr& le, StringView fun_name,
                                    StringView env_name, Type* ret_type,
                                    LambdaCaptureContext& context);
    void backfill_lambda_env(Type* env_type, const LambdaCaptureContext& context);

    // Builders for synthetic AST nodes (capture lowering and self-capture
    // rewriting). Each bump-allocates an Expr and fills in the tagged-union
    // member plus loc/resolved_type.
    Expr* make_identifier_expr(StringView name, Type* type, SourceLocation loc);
    Expr* make_get_expr(Expr* object, StringView name, Type* type, SourceLocation loc);
    Expr* make_this_expr(Type* type, SourceLocation loc);

    // Recursively populates implicit ref-self captures in lambda contexts
    // 0..target_idx (inclusive). Outermost reads `self` directly via ExprThis;
    // every inner one reads via ExprGet(__env, __self) on the next-outer env.
    // Idempotent (skips contexts that already have has_self_capture set).
    void ensure_self_captured_through(u32 target_idx, Type* struct_type, SourceLocation loc);

    SemaContext& m_context;
    BumpAllocator& m_allocator;
    TypeEnv& m_type_env;
    TypeCache& m_types;
    SymbolTable& m_symbols;
    ErrorReporter& m_reporter;
    LifetimeChecker& m_lifetimes;
    FunctionContext& m_function_context;
    Vector<Decl*>& m_synthetic_decls;

    // Counter for unique lambda IDs (used to name synthesized env structs and
    // call functions).
    u32 m_lambda_id_counter = 0;
    Vector<LambdaCaptureContext*> m_lambda_contexts;
};

} // namespace rx

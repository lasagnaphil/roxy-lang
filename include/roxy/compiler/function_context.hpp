#pragma once

#include "roxy/core/types.hpp"
#include "roxy/compiler/lifetime_checker.hpp"

namespace rx {

struct Type;

// Per-function analysis context: the "which kind of body am I inside" slots
// consulted by the statement walkers (return/yield/throw checking). One value
// is live per body being analyzed; nested body analysis (a lambda synthesized
// mid-statement) installs a fresh default context and restores the enclosing
// one on exit — a lambda body is its own function, so it is not a coroutine,
// not inside the enclosing delete destructor, and not inside its finally
// blocks.
struct FunctionContext {
    // Set while analyzing a function returning Coro<T>; yield statements are
    // only legal (and type-checked against coro_yield_type) when set.
    bool in_coroutine = false;
    Type* coro_yield_type = nullptr;

    // Set while analyzing a *delete* (unnamed) destructor body; `throw` is
    // forbidden there. Named destructors are explicitly called and may throw.
    bool in_delete_destructor = false;

    // Depth of enclosing `finally` blocks within this body (statement-scoped:
    // incremented around each finally body); `yield` inside finally is
    // rejected.
    u32 finally_depth = 0;
};

// RAII guard entered at every body-analysis entry point (free function /
// method wrapper, member body, synthesized lambda call function, Phase B
// generic template body): saves the enclosing FunctionContext, installs a
// fresh default one, and bundles the LifetimeChecker's per-function state
// (move states + branch-termination flag) into the same push/pop. Everything
// per-function restores as ONE unit — the coroutine-method diagnostic gap,
// the lambda branch-terminates leak, and the lambda in-delete-destructor
// leak were all "forgot one slot at one entry point" bugs; a single guard
// object leaves no slot to forget.
class FunctionContextScope {
public:
    FunctionContextScope(FunctionContext& context, LifetimeChecker& lifetimes)
        : m_context_slot(context)
        , m_saved(context)
        , m_lifetime_scope(lifetimes) {
        context = FunctionContext{};
    }
    ~FunctionContextScope() { m_context_slot = m_saved; }
    FunctionContextScope(const FunctionContextScope&) = delete;
    FunctionContextScope& operator=(const FunctionContextScope&) = delete;

private:
    FunctionContext& m_context_slot;
    FunctionContext m_saved;
    LifetimeChecker::FunctionScope m_lifetime_scope;
};

}

#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/compiler/error_reporter.hpp"
#include "roxy/compiler/sema_context.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Move state for uniq ownership tracking
enum class MoveState : u8 {
    Live,         // Variable owns a valid value
    Moved,        // Ownership has been transferred (use is an error)
    MaybeValid,   // Conditionally moved (e.g., moved in one branch of if/else)
};

// Move-state map for noncopyable variables (per-function).
// Keyed by Symbol* for correct handling of variable shadowing.
using MoveStateSnapshot = tsl::robin_map<Symbol*, MoveState>;

// LifetimeChecker owns the per-function lifetime analysis that the semantic
// analyzer drives while walking statements: move-state tracking for
// noncopyable variables (use-after-move detection), definite-termination
// tracking for branch merges, cross-iteration loop move checks, and the
// scope-exit named-only-destructor checks. Collaborators are shared by
// reference — the same extraction pattern as TypeChecker/ErrorReporter — so
// it carries no back-reference to the analyzer.
class LifetimeChecker {
public:
    explicit LifetimeChecker(SemaContext& context)
        : m_symbols(context.symbols), m_types(context.types), m_reporter(context.reporter) {}

    // RAII guard for entering a function-body analysis (free function, member
    // body, generic template body, synthesized lambda call function). Saves
    // and clears the move-state map AND the branch-termination flag as a
    // unit, restoring both on destruction. Resetting the termination flag is
    // load-bearing for *nested* body analysis (a lambda synthesized and
    // analyzed mid-statement): a `return` inside the lambda body terminates
    // the lambda, and must not leak "this branch terminates" into the
    // enclosing statement's merge logic — that would discard the enclosing
    // branch's move states at the join point (a use-after-move false
    // negative).
    class FunctionScope {
    public:
        explicit FunctionScope(LifetimeChecker& checker)
            : m_checker(checker)
            , m_saved_states(std::move(checker.m_move_states))
            , m_saved_terminates(checker.m_branch_terminates) {
            checker.m_move_states.clear();  // moved-from map: guarantee empty
            checker.m_branch_terminates = false;
        }
        ~FunctionScope() {
            m_checker.m_move_states = std::move(m_saved_states);
            m_checker.m_branch_terminates = m_saved_terminates;
        }
        FunctionScope(const FunctionScope&) = delete;
        FunctionScope& operator=(const FunctionScope&) = delete;

    private:
        LifetimeChecker& m_checker;
        MoveStateSnapshot m_saved_states;
        bool m_saved_terminates;
    };

    // ===== Move-state tracking =====

    // Begin tracking a noncopyable variable/parameter as Live. Null-safe:
    // callers pass the result of a symbol lookup directly.
    void track_live(Symbol* sym) {
        if (sym) m_move_states[sym] = MoveState::Live;
    }

    // Mark a uniq variable as moved
    void mark_moved(StringView name);

    // Mark a uniq variable as live (for reassignment)
    void mark_live(StringView name);

    // Returns true and writes the current state to `out` if `sym` is
    // move-tracked; false (out untouched) otherwise.
    bool lookup_state(Symbol* sym, MoveState& out) const {
        auto it = m_move_states.find(sym);
        if (it == m_move_states.end()) return false;
        out = it->second;
        return true;
    }

    // Overwrite the state of an already-tracked symbol (no-op if untracked).
    void set_state(Symbol* sym, MoveState state) {
        auto it = m_move_states.find(sym);
        if (it != m_move_states.end()) it.value() = state;
    }

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

    // ===== Definite-termination tracking =====

    // True after analyzing a statement that always transfers control out of
    // the current join point (return/throw/break/continue). Sticky through
    // straight-line blocks; reset inside loop bodies; consumed at if/when/try
    // merge sites to pick the surviving branch's move-state snapshot instead
    // of producing MaybeValid.
    bool branch_terminates() const { return m_branch_terminates; }
    void set_branch_terminates(bool terminates) { m_branch_terminates = terminates; }

    // ===== Branch snapshots and merges =====

    // Save/restore move states for branching (if/else)
    MoveStateSnapshot save_move_states() const { return m_move_states; }
    void restore_move_states(const MoveStateSnapshot& snapshot) { m_move_states = snapshot; }

    // Merge move states from two branches (e.g., if/else)
    void merge_move_states(const MoveStateSnapshot& then_states, const MoveStateSnapshot& else_states);

    // Two-way merge for if/else: keeps the surviving branch's snapshot when one
    // branch terminates, merges both when neither does, and sets
    // branch_terminates() = then_terminates && else_terminates. A no-else `if`
    // calls this with else_states = pre_branch and else_terminates = false.
    void merge_two_branches(const MoveStateSnapshot& pre_branch,
                            const MoveStateSnapshot& then_states,
                            const MoveStateSnapshot& else_states,
                            bool then_terminates, bool else_terminates);

    // N-way merge for when/try: merges every non-terminating snapshot into
    // the current move states. Returns true when every branch terminates (the
    // join point is unreachable), in which case the move states are restored
    // to the first snapshot. Does NOT touch branch_terminates() — callers set
    // it, since when/try diverge afterwards (try has a finally clause).
    bool merge_branch_snapshots(const Vector<MoveStateSnapshot>& snapshots,
                                const Vector<bool>& terminates);

    // ===== Loop checks =====

    // Cross-iteration use-after-move check shared by while/for. A loop-carried
    // noncopyable variable that is Live before the loop but Moved/MaybeValid
    // after the body would be used-after-move on the next iteration — unless the
    // body unconditionally reassigns it first (see loop_reassigns_var_first).
    void check_loop_cross_iteration_moves(Stmt* body,
                                          const MoveStateSnapshot& pre_loop_states,
                                          const MoveStateSnapshot& post_body_states,
                                          SourceLocation loc);

    // ===== Scope-exit destructor checks =====

    // Check live uniq variables at scope exit for named-only destructors
    void check_scope_exit_uniq_destructors(const Scope* scope, SourceLocation loc);
    void check_all_scopes_uniq_destructors(SourceLocation loc, ScopeKind stop_kind);

private:
    // True if `expr` is a reference to an `out`/`inout` parameter — a member of
    // the second-class family that must flow downward only (lifetimes.md "The second-class family").
    // Used to reject escapes (bind-to-ref, store, return, capture). `self` is
    // NOT covered here: it is typed ref<T> and its retention goes through the
    // runtime promotion gate, not a compile error.
    bool is_out_inout_param(Expr* expr);

    // True if the loop `body`'s first executable statement is a plain `=`
    // assignment to `var_name` whose RHS does not reference it. Such a variable
    // is refreshed at the top of every iteration before any use, so it cannot be
    // a cross-iteration use-after-move regardless of the back-edge state.
    // Conservative: only the leading top-level statement is examined.
    bool loop_reassigns_var_first(Stmt* body, StringView var_name) const;

    // True if `expr` (recursively) contains an identifier reference to `name`.
    // Conservative — unknown expression shapes are treated as referencing it.
    bool expr_references_name(Expr* expr, StringView name) const;

    SymbolTable& m_symbols;
    TypeCache& m_types;
    ErrorReporter& m_reporter;

    // Move-state tracking for noncopyable variables (per-function).
    tsl::robin_map<Symbol*, MoveState> m_move_states;

    // See branch_terminates().
    bool m_branch_terminates = false;
};

}

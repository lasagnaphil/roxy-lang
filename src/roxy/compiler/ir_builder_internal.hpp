#pragma once

// File-internal helpers shared by the ir_builder*.cpp translation units
// (ir_builder.cpp / _stmt / _expr / _lifetime). Not part of the compiler's
// public headers — expression-shape and type-shape classification the split
// TUs all consult.

#include "roxy/compiler/ir_builder.hpp"

namespace rx {
namespace ir_builder_detail {

// get_type_slot_count with the historical "0 (null/opaque type) counts as one
// slot" default applied.
inline u32 slot_count_or_1(Type* type) {
    u32 slot_count = get_type_slot_count(type);
    return slot_count == 0 ? 1 : slot_count;
}

// True for types whose value is an owning heap pointer held in a register /
// slot: `uniq T`, List, Map, Coro, and `fun` closures (a uniq env pointer).
// They share teardown shape — load the pointer, typed Delete — and their
// cleanup records are narrowed by a Nullify at the destroy point.
inline bool holds_owning_pointer(Type* type) {
    return type && (type->kind == TypeKind::Uniq || type->kind == TypeKind::Function
                    || type->is_list() || type->is_map() || type->is_coroutine());
}

// Whether a local, parameter, or temporary of this type must be **tracked for
// cleanup** — it carries drop glue that scope exit (and every other exit path)
// has to run.
//
// Deliberately NOT `noncopyable()`, which answers a different question: whether
// binding the value *moves* its source. The two once coincided, and separating
// them is the point of lifetimes.md → "Separating Drop from Copy": a copyable
// struct holding a `string` carries drop glue, so it must be destroyed at scope
// exit while its source stays live. Gating tracking on move-only-ness is what
// silently dropped that destruction on the floor.
//
// `ref` and `string` are excluded because they have their own OwnedKind tracking
// (RefBorrow / StrOwn) at the same sites; this predicate covers the `Owned` kind.
inline bool tracked_for_cleanup(Type* t) {
    if (!t) return false;
    if (t->kind == TypeKind::Ref || t->kind == TypeKind::String) return false;
    return member_needs_drop(t);
}

// Whether this type's counts are acquired somewhere OTHER than the generic
// value-lifecycle glue, so emitting that glue for it would count it twice.
//
// Only `ref` today. A `ref` element or field is incremented by the runtime (a
// map's value_is_ref path) or by an explicit heap-gated inc at the site that
// creates the borrow, both of which predate the glue. Naming it keeps that
// carve-out — and its justification — in one place rather than restated as a
// bare `kind != Ref` at every acquire site.
inline bool counted_by_runtime(Type* t) {
    return t && t->kind == TypeKind::Ref;
}

// Whether a struct rvalue's storage was created by the expression that produced
// it — a struct literal's own stack allocation, or the slot a call returned
// into. Nobody else owns that storage and nothing will ever drop it, so its
// members' counts have to go somewhere: copying out of it is a MOVE, and the
// counts transfer. Any other source (an identifier, a field read, a container
// element) is storage that stays owned by someone else, so copying out of it is
// a CLONE and every counted member needs its own count.
//
// This is the same "produces fresh storage" test gen_var_decl and gen_assign
// already use to decide whether an aliasing copy is needed at all; naming it
// keeps the copy decision and the ownership decision reading off one rule.
inline bool produces_fresh_struct_storage(const Expr* e) {
    return e && (e->kind == AstKind::ExprStructLiteral || e->kind == AstKind::ExprCall);
}

inline StructCopyKind struct_copy_kind_for(const Expr* source) {
    return produces_fresh_struct_storage(source) ? StructCopyKind::Move
                                                 : StructCopyKind::Clone;
}

// Whether a BY-VALUE PARAMETER of this type is the callee's to destroy.
//
// The question is not "does this type carry drop glue" but "did the call site
// hand a count over". It did exactly when the argument was MOVED in — that is,
// when the type is move-only. A copyable argument's slots are duplicated into
// the callee's registers during bytecode lowering with no retain at the call
// site, so the parameter is a BORROW for the call's duration; destroying it
// would release a count the caller still holds and free a live value.
//
// This is already the convention for `string` parameters — a `string` argument
// is passed without a retain and the callee never releases it — and a copyable
// struct that merely *contains* a string is the same case one level up. Taking
// the other branch (callee owns) is possible, but it would mean cloning at every
// by-value struct argument, which is exactly the cost the borrow avoids.
//
// Deliberately NOT `tracked_for_cleanup`: locals and parameters answer different
// questions. A local acquired its own count — the clone glue at its declaration
// put it there — and so must release it. A by-value parameter acquired nothing.
// "Drop where you acquired" is the rule that keeps the two halves inverse, and
// conflating them here is what would turn the leak into a premature free once a
// copyable struct starts carrying drop glue.
//
// `ref`/`string` report false, as they must: both have their own counting at
// this site (m_ref_params' entry RefInc, and nothing at all for `string`).
inline bool param_owns_its_value(Type* t) {
    return t && t->noncopyable();
}

// A `ref`-typed expression "hands off" a borrow count when it is the result of
// a call: by the counting convention (gen_return_stmt) every ref-returning
// function returns with exactly one count handed to the caller. All other ref
// sources (identifiers, borrowed subscripts, `ref x`, field reads) carry no
// count of their own, so binding from them is a fresh borrow that increments.
inline bool is_ref_handoff_source(Expr* init) {
    return init && init->kind == AstKind::ExprCall;
}

// True if `e` is a bare `self` reference (possibly parenthesized). Inside a
// method body `self` is `ExprThis`; inside a lambda body it has already been
// rewritten to `__env.__self` (an `ExprGet` sourced from a heap-checked env), so
// only `ExprThis` is the un-promoted second-class receiver borrow.
inline bool is_bare_self(Expr* e) {
    while (e && e->kind == AstKind::ExprGrouping) e = e->grouping.expr;
    return e && e->kind == AstKind::ExprThis;
}

// For a call argument, the index into the callee's `func_info.param_types` is
// `arg_index + offset`. The layouts, per how semantic analysis types the callee:
//   - identifier callee (free/native function, closure local): the function's
//     own type, no implicit self → 0.
//   - module-qualified callee (`module.fn` — the object resolves to no type):
//     the imported function's own type, no implicit self → 0.
//   - field-stored closure (`obj.callback` where callback is a fun-typed
//     field): the field's plain function type, no implicit self → 0.
//   - genuine method callee — user-struct methods AND List/Map/Coro builtin
//     methods, both typed via build_method_function_type — carries `self` at
//     param_types[0] → 1.
//   - any other callee expression (indirect call through a call result, index,
//     ...): its own plain function type → 0.
// Returns -1 only when the object's type is unresolved and the shape can't be
// classified (defensive; callers skip). Used to heap-gate a bare-`self`
// argument bound to a `ref`/`weak` parameter (lifetimes.md "Promotion") and by
// mark_call_args_moved to align arguments with owned parameters.
inline i32 self_pass_param_offset(CallExpr& call_expr) {
    Expr* callee = call_expr.callee;
    if (!callee) return -1;
    if (callee->kind != AstKind::ExprGet) return 0;
    Expr* obj = callee->get.object;
    if (!obj) return -1;
    if (obj->resolved_type == nullptr) return 0;  // module-qualified: no self
    Type* obj_base = obj->resolved_type->base_type();
    if (!obj_base) return -1;
    if (obj_base->is_struct()) {
        const FieldInfo* fn_field = obj_base->struct_info.find_field(callee->get.name);
        if (fn_field && fn_field->type && fn_field->type->base_type()->is_function())
            return 0;  // field-stored closure: no implicit self
        return 1;  // genuine user-struct method
    }
    if (obj_base->is_container() || obj_base->is_coroutine()) {
        return 1;  // builtin method: build_method_function_type includes self
    }
    return 0;  // any other object shape: the callee carries a plain fn type
}

// The declared parameter type that explicit argument `arg_index` binds to, or
// null when the callee's shape can't be classified (callers then fall back to
// the argument's own type). Wraps self_pass_param_offset's index shift so the
// three call-lowering sites that need a parameter type don't each re-derive it.
//
// The shift is the whole point: a method callee — user-struct methods and
// List/Map/Coro builtin methods alike — carries `self` at param_types[0], so
// explicit argument `i` is param_types[i + 1]. Indexing with a bare `i`
// compared each argument against the *previous* parameter, which is how a
// `uniq`/`ref` passed to a `weak` method parameter (including
// `List<weak T>.push`) once escaped being snapshotted into a {pointer,
// generation} pair and read back as a dangling reference.
inline Type* callee_param_type(CallExpr& call_expr, u32 arg_index) {
    Expr* callee = call_expr.callee;
    Type* fn_type = callee && callee->resolved_type
        ? callee->resolved_type->base_type() : nullptr;
    if (!fn_type || !fn_type->is_function()) return nullptr;
    i32 self_offset = self_pass_param_offset(call_expr);
    if (self_offset < 0) return nullptr;
    Span<Type*> param_types = fn_type->func_info.param_types;
    u32 param_index = arg_index + static_cast<u32>(self_offset);
    return param_index < param_types.size() ? param_types[param_index] : nullptr;
}

}  // namespace ir_builder_detail
}  // namespace rx

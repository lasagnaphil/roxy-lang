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

}  // namespace ir_builder_detail
}  // namespace rx

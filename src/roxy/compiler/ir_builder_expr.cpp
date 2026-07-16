// IRBuilder — expression generation and instruction emission: the emit_*
// helpers, Phase-1 fold/simplify wrappers (pure evaluation in ir_fold.cpp),
// call lowering, and every gen_*_expr. Split out of ir_builder.cpp;
// file-internal helpers shared across the ir_builder*.cpp TUs live in
// ir_builder_internal.hpp.

#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/ir_fold.hpp"
#include "roxy/compiler/operator_traits.hpp"
#include "roxy/compiler/generics.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/map.hpp"

#include "ir_builder_internal.hpp"

#include <cstring>

namespace rx {

using namespace ir_builder_detail;

IRInst* IRBuilder::emit_inst(IROp op, Type* result_type) {
    if (!m_current_block) return nullptr;

    IRInst* inst = m_allocator.emplace<IRInst>();
    inst->op = op;
    inst->result = m_current_func->new_value();
    inst->type = result_type;
    inst->source_line = m_current_source_line;
    m_current_func->values_by_id[inst->result.id] = inst;
    m_current_block->instructions.push_back(inst);
    return inst;
}

ValueId IRBuilder::emit_const_null() {
    IRInst* inst = emit_inst(IROp::ConstNull, m_types.nil_type());
    return inst ? inst->result : ValueId::invalid();
}

ValueId IRBuilder::emit_const_bool(bool value) {
    IRInst* inst = emit_inst(IROp::ConstBool, m_types.bool_type());
    if (inst) {
        inst->const_data.bool_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_int(i64 value, Type* type) {
    if (!type) type = m_types.i64_type();
    // Keep u32 constants canonically zero-extended: constant-folded u32 arithmetic
    // can overflow 32 bits (e.g. 0xFFFFFFFF + 1 -> 0x100000000), and this ConstInt
    // is materialized straight to a register via LOAD_CONST (bypassing lowering's
    // TRUNC_U 32 hook). Masking here wraps it to the correct 32-bit value.
    if (type->kind == TypeKind::U32) {
        value = static_cast<i64>(static_cast<u64>(value) & 0xFFFFFFFFULL);
    }
    IRInst* inst = emit_inst(IROp::ConstInt, type);
    if (inst) {
        inst->const_data.int_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_float(f64 value, Type* type) {
    if (!type) type = m_types.f64_type();

    // Check if this is an f32 - if so, emit ConstF
    if (type->kind == TypeKind::F32) {
        IRInst* inst = emit_inst(IROp::ConstF, type);
        if (inst) {
            inst->const_data.f32_val = static_cast<f32>(value);
            return inst->result;
        }
        return ValueId::invalid();
    }

    // f64 case
    IRInst* inst = emit_inst(IROp::ConstD, type);
    if (inst) {
        inst->const_data.f64_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_string(StringView value) {
    IRInst* inst = emit_inst(IROp::ConstString, m_types.string_type());
    if (inst) {
        inst->const_data.string_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_folded_const(const FoldedConst& folded, Type* result_type) {
    switch (folded.kind) {
        case FoldedConst::Kind::Int:   return emit_const_int(folded.int_val, result_type);
        case FoldedConst::Kind::Bool:  return emit_const_bool(folded.bool_val);
        case FoldedConst::Kind::Float: return emit_const_float(folded.float_val, result_type);
    }
    return ValueId::invalid();
}

ValueId IRBuilder::try_fold_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    FoldedConst folded;
    if (!fold_binary_const(op, m_current_func->inst_for(left),
                           m_current_func->inst_for(right), folded)) {
        return ValueId::invalid();
    }
    return emit_folded_const(folded, result_type);
}

ValueId IRBuilder::try_simplify_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    IRInst* l = m_current_func->inst_for(left);
    IRInst* r = m_current_func->inst_for(right);

    auto is_ci = [](IRInst* i, i64 val) {
        return i && i->op == IROp::ConstInt && i->const_data.int_val == val;
    };

    switch (op) {
    case IROp::AddI:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        return ValueId::invalid();

    case IROp::SubI:
        if (is_ci(r, 0)) return left;
        if (left == right) return emit_const_int(0, result_type);
        return ValueId::invalid();

    case IROp::MulI:
        if (is_ci(r, 0)) return emit_const_int(0, result_type);
        if (is_ci(l, 0)) return emit_const_int(0, result_type);
        if (is_ci(r, 1)) return left;
        if (is_ci(l, 1)) return right;
        if (is_ci(r, 2)) return emit_binary(IROp::AddI, left, left, result_type);
        if (is_ci(l, 2)) return emit_binary(IROp::AddI, right, right, result_type);
        return ValueId::invalid();

    case IROp::DivI:
        if (is_ci(r, 1)) return left;
        return ValueId::invalid();

    case IROp::BitAnd:
        if (is_ci(r, 0)) return emit_const_int(0, result_type);
        if (is_ci(l, 0)) return emit_const_int(0, result_type);
        if (is_ci(r, -1)) return left;
        if (is_ci(l, -1)) return right;
        return ValueId::invalid();

    case IROp::BitOr:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        if (is_ci(r, -1)) return emit_const_int(-1, result_type);
        if (is_ci(l, -1)) return emit_const_int(-1, result_type);
        return ValueId::invalid();

    case IROp::BitXor:
        if (is_ci(r, 0)) return left;
        if (is_ci(l, 0)) return right;
        if (left == right) return emit_const_int(0, result_type);
        return ValueId::invalid();

    case IROp::Shl:
    case IROp::Shr:
        if (is_ci(r, 0)) return left;
        return ValueId::invalid();

    default:
        return ValueId::invalid();
    }
}

ValueId IRBuilder::emit_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    if (ValueId folded = try_fold_binary(op, left, right, result_type); folded.is_valid()) {
        return folded;
    }
    if (ValueId simplified = try_simplify_binary(op, left, right, result_type); simplified.is_valid()) {
        return simplified;
    }
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->binary.left = left;
        inst->binary.right = right;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::try_fold_unary(IROp op, ValueId operand, Type* result_type) {
    if (!m_current_func) return ValueId::invalid();
    FoldedConst folded;
    if (!fold_unary_const(op, m_current_func->inst_for(operand), folded)) {
        return ValueId::invalid();
    }
    return emit_folded_const(folded, result_type);
}

ValueId IRBuilder::try_simplify_unary(IROp op, ValueId operand, Type* result_type) {
    (void)result_type;
    if (!m_current_func) return ValueId::invalid();
    IRInst* o = m_current_func->inst_for(operand);
    if (!o) return ValueId::invalid();

    // Double-negation: only safe for integer / bool / bitwise. Float Neg is
    // skipped because -(-0.0) = 0.0 distinguishes from -0.0 in IEEE-754.
    if ((op == IROp::NegI && o->op == IROp::NegI) ||
        (op == IROp::Not && o->op == IROp::Not) ||
        (op == IROp::BitNot && o->op == IROp::BitNot)) {
        return o->unary;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_unary(IROp op, ValueId operand, Type* result_type) {
    if (ValueId folded = try_fold_unary(op, operand, result_type); folded.is_valid()) {
        return folded;
    }
    if (ValueId simplified = try_simplify_unary(op, operand, result_type); simplified.is_valid()) {
        return simplified;
    }
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->unary = operand;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_copy(ValueId value, Type* type) {
    IRInst* inst = emit_inst(IROp::Copy, type);
    if (inst) {
        inst->unary = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call(StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::Call, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = 0;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_native(StringView func_name, Span<ValueId> args, Type* result_type, u32 native_index) {
    IRInst* inst = emit_inst(IROp::CallNative, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = native_index;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_external(StringView module_name, StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::CallExternal, result_type);
    if (inst) {
        inst->call_external.module_name = module_name;
        inst->call_external.func_name = func_name;
        inst->call_external.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_index_get(ValueId container, ValueId index, ContainerKind kind, Type* result_type) {
    IRInst* inst = emit_inst(IROp::IndexGet, result_type);
    if (inst) {
        inst->index_data.container = container;
        inst->index_data.index = index;
        inst->index_data.value = ValueId::invalid();
        inst->index_data.kind = kind;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_index_set(ValueId container, ValueId index, ValueId value, ContainerKind kind) {
    IRInst* inst = emit_inst(IROp::IndexSet, m_types.void_type());
    if (inst) {
        inst->index_data.container = container;
        inst->index_data.index = index;
        inst->index_data.value = value;
        inst->index_data.kind = kind;
    }
}

ValueId IRBuilder::emit_index_addr(ValueId container, ValueId index, ContainerKind kind, Type* result_type) {
    IRInst* inst = emit_inst(IROp::IndexAddr, result_type);
    if (inst) {
        inst->index_data.container = container;
        inst->index_data.index = index;
        inst->index_data.value = ValueId::invalid();
        inst->index_data.kind = kind;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_new(StringView type_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::New, result_type);
    if (inst) {
        inst->new_data.type_name = type_name;
        inst->new_data.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_stack_alloc(u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StackAlloc, result_type);
    if (inst) {
        inst->stack_alloc.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field_addr(ValueId object, StringView field_name, u32 slot_offset, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetFieldAddr, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = 0;  // Not used for address computation
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_set_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, ValueId value, Type* result_type) {
    IRInst* inst = emit_inst(IROp::SetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        inst->store_value = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_load_ptr(ValueId ptr, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::LoadPtr, result_type);
    if (inst) {
        inst->load_ptr.ptr = ptr;
        inst->load_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_store_ptr(ValueId ptr, ValueId value, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StorePtr, result_type);
    if (inst) {
        inst->store_ptr.ptr = ptr;
        inst->store_ptr.value = value;
        inst->store_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_struct_copy(ValueId dest_ptr, ValueId source_ptr, u32 slot_count) {
    IRInst* inst = emit_inst(IROp::StructCopy, nullptr);
    if (inst) {
        inst->struct_copy.dest_ptr = dest_ptr;
        inst->struct_copy.source_ptr = source_ptr;
        inst->struct_copy.slot_count = slot_count;
    }
}

void IRBuilder::emit_delete(ValueId value, Type* type) {
    IRInst* inst = emit_inst(IROp::Delete, type);
    if (inst) inst->unary = value;
}

void IRBuilder::emit_nullify(ValueId value) {
    IRInst* inst = emit_inst(IROp::Nullify, m_types.void_type());
    if (inst) inst->unary = value;
}

void IRBuilder::emit_assert_heap(ValueId value) {
    IRInst* inst = emit_inst(IROp::AssertHeap, m_types.void_type());
    if (inst) inst->unary = value;
}

void IRBuilder::emit_ref_inc(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefInc, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_ref_dec(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefDec, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_str_retain(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::StrRetain, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_str_release(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::StrRelease, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::maybe_str_retain(ValueId val, Type* type) {
    if (type && type->kind == TypeKind::String) emit_str_retain(val);
}

void IRBuilder::emit_map_value_delete_if_present(ValueId map_obj, Type* map_type, ValueId key_val) {
    if (!map_type || !map_type->is_map()) return;
    Type* value_type = map_type->map_info.value_type;
    // Only noncopyable (uniq / container / struct-with-dtor) values need a typed
    // delete here. A `ref` value is released by the runtime value_is_ref path
    // (roxy_map_insert/remove); a trivial value needs no cleanup.
    if (!value_type || value_type->is_copy()) return;

    StringView contains_native;
    for (const MethodInfo& method : map_type->map_info.methods) {
        if (method.name == "contains"_sv) { contains_native = method.native_name; break; }
    }
    i32 contains_idx = contains_native.empty() ? -1 : m_registry.get_index(contains_native);
    if (contains_idx < 0) return;

    // if (map.contains(key)) { delete map[key]; }
    ValueId present = emit_call_native(contains_native, alloc_span({map_obj, key_val}),
                                       m_types.bool_type(), static_cast<u32>(contains_idx));
    IRBlock* destroy_block = create_block("map_destroy_old");
    IRBlock* merge_block = create_block("map_after_destroy");
    finish_block_branch(present, destroy_block->id, merge_block->id);

    set_current_block(destroy_block);
    ValueId old = emit_index_get(map_obj, key_val, ContainerKind::Map, value_type);
    emit_delete(old, value_type);
    finish_block_goto(merge_block->id);

    set_current_block(merge_block);
}

void IRBuilder::emit_map_clear_value_cleanup(ValueId map_obj, Type* map_type) {
    if (!map_type || !map_type->is_map()) return;
    Type* value_type = map_type->map_info.value_type;
    if (!value_type || value_type->is_copy()) return;  // ref/trivial: nothing to do here

    // Look up the internal iteration natives (pre-provided for exactly this:
    // cleanup of noncopyable map values). If any is missing, skip (the values then
    // leak on clear, as before — no worse than the prior behavior).
    StringView cap_name("__map_iter_capacity", 19);
    StringView next_name("__map_iter_next_occupied", 24);
    StringView val_name("__map_iter_value_at", 19);
    i32 cap_idx = m_registry.get_index(cap_name);
    i32 next_idx = m_registry.get_index(next_name);
    i32 val_idx = m_registry.get_index(val_name);
    if (cap_idx < 0 || next_idx < 0 || val_idx < 0) return;

    Type* i32_type = m_types.i32_type();

    // cap = __map_iter_capacity(map)   (loop-invariant; dominates the loop)
    ValueId cap = emit_call_native(cap_name, alloc_span({map_obj}), i32_type,
                                   static_cast<u32>(cap_idx));

    // Counted loop over occupied buckets:
    //   for (idx = 0; (next = next_occupied(map, idx)) < cap; idx = next + 1)
    //       delete value_at(map, next);
    // `idx` is the only loop-carried value, so it is the header's one block param;
    // `cap`/`next` are used by dominance (no extra args), matching gen_for_stmt.
    IRBlock* header = create_block("mapclr");
    IRBlock* body = create_block("mapclrbody");
    IRBlock* exit_block = create_block("mapclrend");

    ValueId zero = emit_const_int(0, i32_type);
    ValueId idx_param = m_current_func->new_value();
    header->params.push_back({idx_param, i32_type, "__mapclr_idx"_sv});

    Vector<BlockArgPair> init_args; init_args.push_back({zero});
    finish_block_goto(header->id, alloc_span(init_args));

    set_current_block(header);
    ValueId next = emit_call_native(next_name, alloc_span({map_obj, idx_param}), i32_type,
                                    static_cast<u32>(next_idx));
    ValueId cond = emit_binary(IROp::LtI, next, cap, m_types.bool_type());  // next < cap
    finish_block_branch(cond, body->id, exit_block->id);

    set_current_block(body);
    // Type the result as the value type so both backends treat the returned
    // pointer correctly (the C backend casts the u64 to the value's C type).
    ValueId vp = emit_call_native(val_name, alloc_span({map_obj, next}), value_type,
                                  static_cast<u32>(val_idx));
    emit_delete(vp, value_type);
    ValueId one = emit_const_int(1, i32_type);
    ValueId idx_next = emit_binary(IROp::AddI, next, one, i32_type);
    Vector<BlockArgPair> back_args; back_args.push_back({idx_next});
    finish_block_goto(header->id, alloc_span(back_args));

    set_current_block(exit_block);
}

bool IRBuilder::is_map_insert_noncopyable_value(CallExpr& call_expr) const {
    if (!call_expr.callee || call_expr.callee->kind != AstKind::ExprGet) return false;
    GetExpr& get_expr = call_expr.callee->get;
    if (get_expr.name != "insert"_sv) return false;
    Type* obj_type = get_expr.object ? get_expr.object->resolved_type : nullptr;
    Type* base = obj_type ? obj_type->base_type() : nullptr;
    if (!base || !base->is_map()) return false;
    Type* vt = base->map_info.value_type;
    return vt && vt->noncopyable();
}

void IRBuilder::emit_container_pin(ValueId container) {
    IRInst* inst = emit_inst(IROp::ContainerPin, m_types.void_type());
    if (inst) inst->unary = container;
}

void IRBuilder::emit_container_unpin(ValueId container) {
    IRInst* inst = emit_inst(IROp::ContainerUnpin, m_types.void_type());
    if (inst) inst->unary = container;
}

ValueId IRBuilder::emit_pinned_copy(ValueId src, Type* type) {
    IRInst* inst = emit_inst(IROp::Copy, type);
    if (!inst) return src;
    inst->unary = src;
    inst->no_copy_prop = true;
    return inst->result;
}

void IRBuilder::emit_ref_param_decrements() {
    // Emit RefDec for all ref-typed parameters before function exit (normal
    // path). The exception-unwind path is covered separately by RefDec cleanup
    // records (end_function_body) — the two are mutually exclusive per control
    // path, so a param is decremented exactly once.
    for (const RefParamInfo& param : m_ref_params) {
        emit_ref_dec(param.value);
    }
}

ValueId IRBuilder::emit_weak_create(ValueId ptr, Type* weak_type) {
    return emit_unary(IROp::WeakCreate, ptr, weak_type);
}

ValueId IRBuilder::maybe_wrap_weak(ValueId value, Type* source_type, Type* target_type,
                                   Expr* source_expr) {
    if (!source_type || !target_type) return value;
    // A function value is a heap env pointer with a header, so `fun -> weak fun`
    // is created the same way as uniq/ref -> weak.
    if (target_type->kind == TypeKind::Weak &&
        (source_type->kind == TypeKind::Uniq || source_type->kind == TypeKind::Ref ||
         source_type->kind == TypeKind::Function)) {
        // Self-promotion gate: a bare `self` source may be a stack receiver, so
        // trap before WeakCreate snapshots a bogus generation from stack bytes
        // (lifetimes.md "Promotion"). Only bare `self` (ExprThis) is gated — a
        // lambda body's `self` is already `__env.__self` (heap-checked env), and
        // call-arg / capture sites pass source_expr = nullptr because they gate
        // separately. Mirrors emit_ref_borrow_inc's gate on the `ref` side.
        if (source_expr && is_bare_self(source_expr)) {
            emit_assert_heap(value);
        }
        return emit_weak_create(value, target_type);
    }
    return value;
}

// Statement generation

void IRBuilder::emit_ref_borrow_inc(ValueId val, Expr* source) {
    if (is_bare_self(source)) {
        // Self-promotion gate: trap if the receiver is stack-allocated, before
        // the inc writes into a bogus (stack-relative) object header.
        emit_assert_heap(val);
    }
    emit_ref_inc(val);
}

ValueId IRBuilder::gen_expr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprLiteral:
            return gen_literal_expr(expr);
        case AstKind::ExprIdentifier:
            return gen_identifier_expr(expr);
        case AstKind::ExprUnary:
            return gen_unary_expr(expr);
        case AstKind::ExprBinary: {
            ValueId result = gen_binary_expr(expr);
            // String concat (`a + b`) produces a fresh owned string; track it so
            // it's released at scope exit unless consumed (finding 9b).
            track_string_temp(result, expr->resolved_type);
            return result;
        }
        case AstKind::ExprTernary:
            return gen_ternary_expr(expr);
        case AstKind::ExprCall: {
            ValueId result = gen_call_expr(expr);
            // A call returning a noncopyable value (a closure, uniq, list, map…)
            // produces an owned temporary. Track it so it's cleaned at scope exit
            // when used inline and not bound/consumed (e.g. `f()()`,
            // `make_list().len()`). Constructor/struct-literal paths already
            // self-track, so skip if already a tracked temp.
            track_noncopyable_call_temp(result, expr->resolved_type);
            // A string-returning call (native producer or a user function, which
            // hands off an owned count-1 via gen_return) yields an owned string
            // temp — track it for release (finding 9b).
            track_string_temp(result, expr->resolved_type);
            return result;
        }
        case AstKind::ExprIndex:
            return gen_index_expr(expr);
        case AstKind::ExprGet:
            return gen_get_expr(expr);
        case AstKind::ExprAssign:
            return gen_assign_expr(expr);
        case AstKind::ExprGrouping:
            return gen_grouping_expr(expr);
        case AstKind::ExprThis:
            return gen_this_expr(expr);
        case AstKind::ExprStructLiteral:
            return gen_struct_literal_expr(expr);
        case AstKind::ExprStaticGet:
            return gen_static_get_expr(expr);
        case AstKind::ExprStringInterp: {
            ValueId result = gen_string_interp_expr(expr);
            // An f-string builds a fresh owned string — track it (finding 9b).
            track_string_temp(result, expr->resolved_type);
            return result;
        }
        case AstKind::ExprLambda:
            return gen_lambda_expr(expr);
        default:
            report_error("Internal error: unknown expression kind in IR generation");
            return ValueId::invalid();
    }
}

ValueId IRBuilder::gen_lambda_expr(Expr* expr) {
    LambdaExpr& le = expr->lambda;

    // Record the env struct so a synthesized destructor is built for it (after
    // all bodies). A closure value is a uniq env pointer; on delete we dispatch
    // this destructor by the env's runtime type_id to clean its captures.
    if (le.env_struct_type && le.env_struct_type->is_struct()) {
        bool seen = false;
        for (Type* t : m_env_struct_types) {
            if (t == le.env_struct_type) { seen = true; break; }
        }
        if (!seen) m_env_struct_types.push_back(le.env_struct_type);
    }

    // The lambda expression's resolved type is `Function<sig>`. Lowering treats it
    // as a `uniq` pointer to the synthesized env struct.
    //
    // The synthesized call function is non-pub, so build_function applies
    // mangle_module_local to its IRFunction name. We must reference it by the
    // mangled name to find it in the bytecode lowering's m_func_indices map.
    //
    // Captures: emit gen_expr on a synthetic IdentifierExpr per capture (resolved
    // via the IR builder's local-scope map — captures live in the OUTER function
    // we're currently emitting IR for). For Move-mode captures, null-ify the
    // local + Nullify the cleanup-record register (mirrors arg-passing pattern in
    // gen_call_expr around line 3475-3494).
    Vector<ValueId> capture_values;
    capture_values.reserve(le.resolved_captures.size());
    for (const CaptureInfo& cap : le.resolved_captures) {
        // The capture's source expression was built at semantic time. For
        // top-level captures it's a direct IdentifierExpr; for nested closures
        // it's an ExprGet on the enclosing lambda's __env. For self captures
        // it's an ExprThis (or a synthesized struct literal for [copy self]).
        // gen_expr produces a value in the enclosing function's IR scope.
        ValueId v = gen_expr(cap.source_expr);

        // Runtime heap check for self captures whose receiver may be stack-
        // allocated (copyable struct + ref/weak self capture). The check fires
        // on the source pointer before we store it into the env field.
        if (cap.needs_heap_check) {
            emit_assert_heap(v);
        }

        // A `ref` capture (a [ref self] promotion or a captured ref local) is a
        // counted borrow the env now holds — increment it (after the heap check
        // has proven a copyable receiver heap-resident, so a stack receiver traps
        // before the inc). The env destructor decrements it (emit_field_cleanup).
        if (cap.type && cap.type->kind == TypeKind::Ref) {
            emit_ref_inc(v);
        }

        // A `weak` capture ([weak self], or a captured weak local) must store a
        // proper {ptr, generation} snapshot into the env, not a bare receiver
        // pointer — otherwise a later WeakCheck on the captured `weak self` reads
        // a garbage generation. Snapshot via WeakCreate (a no-op if the source is
        // already a weak). The heap check above ran first, so a stack receiver has
        // already trapped by here. (The C backend also stores a proper weak: its
        // Closure emitter only wraps a capture whose value isn't already weak.)
        if (cap.type && cap.type->kind == TypeKind::Weak) {
            v = maybe_wrap_weak(v, cap.source_expr->resolved_type, cap.type);
        }

        capture_values.push_back(v);

        if (cap.mode == CaptureMode::Move) {
            // The capture transfers ownership of the outer value into the env.
            // Suppress scope-exit cleanup of the source so it isn't freed twice.
            // A direct capture sources a local (mark it moved); a *transitive*
            // move sources an enclosing env field (__env.c) — null that field so
            // the enclosing env's destructor doesn't re-delete the moved-out
            // value. (Closure-env cleanup now runs, so this double-free, formerly
            // masked by the inert closure delete, must be prevented.)
            mark_moved_from(cap.name);
            nullify_moved_field_source(cap.source_expr);
        }
    }

    IRInst* inst = emit_inst(IROp::Closure, expr->resolved_type);
    if (!inst) return ValueId::invalid();
    inst->closure.env_struct_name = le.env_struct_name;
    inst->closure.call_function_name = mangle_module_local(le.call_function_name);
    inst->closure.captures = m_allocator.alloc_span(capture_values);
    return inst->result;
}

ValueId IRBuilder::gen_function_ref(Expr* expr, const FunctionRefTarget& target) {
    if (!target.function_type || !target.function_type->is_function()) {
        report_error("Internal error: gen_function_ref target has non-function type");
        return ValueId::invalid();
    }

    // Cache key: target.name is unique per IR-level function (mangled scripts,
    // monomorphized generics) or per native registry entry, so it dedupes
    // across alias paths. Imported script trampolines key on the unique
    // module::name form composed below.
    StringView cache_key = target.name;
    StringView ext_module_name;
    if (target.kind == FunctionRefTarget::Kind::ImportedScript) {
        u32 mod_len = target.module_name.size();
        u32 nm_len = target.name.size();
        u32 total = mod_len + 2 + nm_len;
        char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total, 1));
        memcpy(buf, target.module_name.data(), mod_len);
        buf[mod_len] = ':'; buf[mod_len + 1] = ':';
        memcpy(buf + mod_len + 2, target.name.data(), nm_len);
        cache_key = StringView(buf, total);
        ext_module_name = target.module_name;
    }

    auto cache_it = m_function_refs.find(cache_key);
    StringView env_struct_name;
    StringView trampoline_name;
    if (cache_it != m_function_refs.end()) {
        env_struct_name = cache_it->second.env_struct_name;
        trampoline_name = cache_it->second.trampoline_name;
    } else {
        u32 ref_id = m_funref_id_counter++;

        // Synthesize an empty env struct (just `__call_idx: u32`). Same shape
        // as a zero-capture lambda's env, just a different name so each
        // function-ref maps to its own type_id.
        env_struct_name = intern_format("__funref_{}_env", ref_id);

        Type* env_type = m_types.struct_type(env_struct_name, nullptr);
        FieldInfo* fields = reinterpret_cast<FieldInfo*>(
            m_allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        fields[0].name = "__call_idx"_sv;
        fields[0].type = m_types.u32_type();
        fields[0].is_pub = false;
        fields[0].index = 0;
        fields[0].slot_offset = 0;
        fields[0].slot_count = 1;
        env_type->struct_info.fields = Span<FieldInfo>(fields, 1);
        env_type->struct_info.slot_count = 1;
        env_type->struct_info.constructors = Span<ConstructorInfo>();
        env_type->struct_info.destructors = Span<DestructorInfo>();
        env_type->struct_info.methods = Span<MethodInfo>();
        env_type->struct_info.when_clauses = Span<WhenClauseInfo>();
        env_type->struct_info.implemented_traits = Span<TraitImplRecord>();
        env_type->struct_info.parent = nullptr;
        env_type->struct_info.module_name = StringView(nullptr, 0);
        m_type_env.register_named_type(env_struct_name, env_type);
        // Track for the C backend (typedef + TYPEID), like lambda env structs.
        m_env_struct_types.push_back(env_type);

        // Synthesize the trampoline IRFunction. Following the convention in
        // build_function (line 365-374), non-pub functions get
        // mangle_module_local applied; "main" stays unmangled. The trampoline
        // is non-pub by construction, so always mangle.
        StringView raw_tramp_name = intern_format("__funref_{}_call", ref_id);
        trampoline_name = mangle_module_local(raw_tramp_name);

        FunctionTypeInfo& fti = target.function_type->func_info;
        Type* ref_env_type = m_types.ref_type(env_type);

        // Build the IRFunction in a hand-rolled fashion (mirrors the pattern
        // in coroutine_lowering.cpp lines 996-1021). We must NOT use IRBuilder's
        // emit_* helpers because those mutate m_current_func / m_current_block,
        // which are owned by the caller's enclosing function-IR generation.
        IRFunction* tramp = m_allocator.emplace<IRFunction>();
        tramp->name = trampoline_name;
        tramp->return_type = fti.return_type;

        // Param 0: __env: ref EnvType (unused inside the body).
        BlockParam env_param;
        env_param.value = tramp->new_value();
        env_param.type = ref_env_type;
        env_param.name = "__env"_sv;
        tramp->params.push_back(env_param);
        tramp->param_is_ptr.push_back(false);

        // Params 1..N: forwarded arguments named arg0, arg1, ...
        Vector<ValueId> forwarded_arg_values;
        for (u32 i = 0; i < fti.param_types.size(); i++) {
            StringView arg_name = intern_format("arg{}", i);

            BlockParam p;
            p.value = tramp->new_value();
            p.type = fti.param_types[i];
            p.name = arg_name;
            tramp->params.push_back(p);
            tramp->param_is_ptr.push_back(false);
            forwarded_arg_values.push_back(p.value);
        }

        // Single entry block: <body call>; Return result. The body op is
        // chosen by target kind so the same trampoline shell forwards to
        // script / native / external functions uniformly.
        IRBlock* entry = m_allocator.emplace<IRBlock>();
        entry->id = BlockId{0};
        entry->name = "entry"_sv;
        tramp->blocks.push_back(entry);

        IRInst* call_inst = m_allocator.emplace<IRInst>();
        call_inst->type = fti.return_type;
        call_inst->result = tramp->new_value();
        tramp->values_by_id[call_inst->result.id] = call_inst;
        switch (target.kind) {
            case FunctionRefTarget::Kind::Script:
                call_inst->op = IROp::Call;
                call_inst->call.func_name = target.name;
                call_inst->call.args = m_allocator.alloc_span(forwarded_arg_values);
                call_inst->call.native_index = 0;
                break;
            case FunctionRefTarget::Kind::Native:
            case FunctionRefTarget::Kind::ImportedNative:
                call_inst->op = IROp::CallNative;
                call_inst->call.func_name = target.name;
                call_inst->call.args = m_allocator.alloc_span(forwarded_arg_values);
                call_inst->call.native_index = target.native_index;
                break;
            case FunctionRefTarget::Kind::ImportedScript:
                call_inst->op = IROp::CallExternal;
                call_inst->call_external.module_name = ext_module_name;
                call_inst->call_external.func_name = target.name;
                call_inst->call_external.args = m_allocator.alloc_span(forwarded_arg_values);
                break;
        }
        entry->instructions.push_back(call_inst);

        // Return the call's result (or void).
        entry->terminator.kind = TerminatorKind::Return;
        if (fti.return_type && !fti.return_type->is_void()) {
            entry->terminator.return_value = call_inst->result;
        } else {
            entry->terminator.return_value = ValueId::invalid();
        }

        m_module->functions.push_back(tramp);

        FunctionRefInfo info{env_struct_name, trampoline_name};
        m_function_refs[cache_key] = info;
    }

    IRInst* inst = emit_inst(IROp::Closure, expr->resolved_type);
    if (!inst) return ValueId::invalid();
    inst->closure.env_struct_name = env_struct_name;
    inst->closure.call_function_name = trampoline_name;
    inst->closure.captures = Span<ValueId>();
    return inst->result;
}

ValueId IRBuilder::gen_literal_expr(Expr* expr) {
    LiteralExpr& lit = expr->literal;

    switch (lit.literal_kind) {
        case LiteralKind::Nil:
            return emit_const_null();
        case LiteralKind::Bool:
            return emit_const_bool(lit.bool_value);
        case LiteralKind::I32:
        case LiteralKind::I64:
        case LiteralKind::U32:
        case LiteralKind::U64: {
            // Safety net: if IntLiteral wasn't concretized, default to i32
            Type* int_type = expr->resolved_type;
            if (int_type && int_type->is_int_literal()) {
                int_type = m_types.i32_type();
            }
            return emit_const_int(lit.int_value, int_type);
        }
        case LiteralKind::F32:
        case LiteralKind::F64:
            return emit_const_float(lit.float_value, expr->resolved_type);
        case LiteralKind::String:
            return emit_const_string(lit.string_value);
    }
    report_error("Internal error: unknown literal kind in IR generation");
    return ValueId::invalid();
}

ValueId IRBuilder::gen_identifier_expr(Expr* expr) {
    IdentifierExpr& id = expr->identifier;

    // Function reference: if the identifier doesn't resolve to a local but does
    // resolve to a top-level function symbol, materialize a closure value
    // wrapping a synthesized trampoline. gen_call_expr's direct-call path
    // doesn't recurse here for callee identifiers, so this only fires in value
    // contexts (var init, argument passing, return, struct literal field, ...).
    LocalVar* lv = find_local(id.name);
    if (!lv) {
        // Generic-template ref: semantic analysis stashed the monomorphized
        // name on the identifier post-coercion. The instantiated function
        // type lives in expr->resolved_type. Apply module-local mangling
        // when the template is non-pub, mirroring what build_function does
        // when emitting the IR for the instance.
        if (id.mangled_name.size() > 0) {
            FunctionRefTarget target;
            target.kind = FunctionRefTarget::Kind::Script;
            target.function_type = expr->resolved_type;
            bool template_is_pub = false;
            if (Decl* tdecl = m_type_env.generics().get_generic_fun_decl(id.name);
                tdecl && tdecl->kind == AstKind::DeclFun) {
                template_is_pub = tdecl->fun_decl.is_pub;
            }
            target.name = template_is_pub
                ? id.mangled_name
                : mangle_module_local(id.mangled_name);
            return gen_function_ref(expr, target);
        }
        Symbol* sym = m_symbols.lookup(id.name);
        if (sym && sym->kind == SymbolKind::Function) {
            FunctionRefTarget target;
            target.function_type = sym->type;
            bool is_native = sym->decl && sym->decl->kind == AstKind::DeclFun
                && sym->decl->fun_decl.is_native;
            if (is_native) {
                target.kind = FunctionRefTarget::Kind::Native;
                target.name = sym->name;
                i32 idx = m_registry.get_index(sym->name);
                if (idx < 0) {
                    report_error("Internal error: native function not in registry");
                    return ValueId::invalid();
                }
                target.native_index = static_cast<u32>(idx);
            } else {
                target.kind = FunctionRefTarget::Kind::Script;
                target.name = sym->name;
                if (!sym->is_pub && sym->name != "main"_sv) {
                    target.name = mangle_module_local(sym->name);
                }
            }
            return gen_function_ref(expr, target);
        }
        if (sym && sym->kind == SymbolKind::ImportedFunction) {
            FunctionRefTarget target;
            target.function_type = sym->type;
            if (sym->imported_func.is_native) {
                target.kind = FunctionRefTarget::Kind::ImportedNative;
                target.name = sym->imported_func.original_name;
                i32 idx = m_registry.get_index(target.name);
                if (idx < 0) {
                    report_error("Internal error: imported native not in registry");
                    return ValueId::invalid();
                }
                target.native_index = static_cast<u32>(idx);
            } else {
                target.kind = FunctionRefTarget::Kind::ImportedScript;
                target.name = sym->imported_func.original_name;
                target.module_name = sym->imported_func.module_name;
            }
            return gen_function_ref(expr, target);
        }

        // Module-level global read: not a local, not a function — load from the
        // global slot (a local of the same name would have set `lv` and skipped
        // this whole block, so a local correctly shadows a global).
        auto git = m_global_indices.find(id.name);
        if (git != m_global_indices.end()) {
            return gen_global_read(git->second, expr->resolved_type);
        }

        // Not a local, function, or global: undefined. lookup_local reports the
        // internal error and returns invalid (sema should have caught this).
        return lookup_local(id.name);
    }

    // Local variable: reuse the LocalVar found above — no second scope walk (was
    // lookup_local) and no m_param_is_ptr hash probe (is_ptr is cached). (§3.7)
    ValueId val = lv->value;

    // If this is a pointer parameter (out/inout), handle specially
    if (lv->is_ptr) {
        Type* type = expr->resolved_type;

        // For struct types, the pointer IS what we need for field access.
        // Don't dereference - struct operations (GET_FIELD, SET_FIELD) expect pointers.
        if (type && type->is_struct()) {
            return val;
        }

        // For primitive types and pointer-sized types, dereference the pointer to get the value.
        // get_type_slot_count covers every non-struct width (incl. weak=4, uniq/ref/fn=2);
        // structs returned above. 0 means null/opaque type — preserve the prior 1-slot default.
        u32 slot_count = slot_count_or_1(type);
        return emit_load_ptr(val, slot_count, type);
    }

    return val;
}

ValueId IRBuilder::gen_unary_expr(Expr* expr) {
    UnaryExpr& unary_expr = expr->unary;

    // Check for struct unary operator trait dispatch
    Type* operand_type = unary_expr.operand->resolved_type;
    if (operand_type && operand_type->is_struct()) {
        StringView method_name = unary_op_to_trait_method(unary_expr.op);
        if (!method_name.empty()) {
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(operand_type, method_name, &found_in);
            if (mi && found_in) {
                ValueId self_ptr = gen_lvalue_addr(unary_expr.operand);
                StringView mangled = mangle_method(found_in->struct_info.name, method_name);
                return emit_call(mangled, alloc_span({self_ptr}), expr->resolved_type);
            }
        }
    }

    // ref expr: borrow a reference — just pass through the value with ref type
    if (unary_expr.op == UnaryOp::Ref) {
        ValueId operand = gen_expr(unary_expr.operand);
        Type* result_type = expr->resolved_type;
        // uniq and ref have the same runtime representation, just reinterpret the type
        return emit_unary(IROp::Copy, operand, result_type);
    }

    ValueId operand = gen_expr(unary_expr.operand);
    Type* result_type = expr->resolved_type;

    IROp op = get_unary_op(unary_expr.op, operand_type);
    return emit_unary(op, operand, result_type);
}

ValueId IRBuilder::gen_binary_expr(Expr* expr) {
    BinaryExpr& binary_expr = expr->binary;

    // Reference/nil comparison: ref/uniq/weak == nil or != nil
    Type* left_type = binary_expr.left->resolved_type;
    Type* right_type = binary_expr.right->resolved_type;
    if ((binary_expr.op == BinaryOp::Equal || binary_expr.op == BinaryOp::NotEqual) &&
        ((left_type && left_type->is_reference() && right_type && right_type->is_nil()) ||
         (left_type && left_type->is_nil() && right_type && right_type->is_reference()))) {
        ValueId ref_val, null_val;
        if (left_type->is_reference()) {
            ref_val = gen_expr(binary_expr.left);
            null_val = emit_const_null();
        } else {
            null_val = emit_const_null();
            ref_val = gen_expr(binary_expr.right);
        }
        IROp cmp_op = (binary_expr.op == BinaryOp::Equal) ? IROp::EqI : IROp::NeI;
        return emit_binary(cmp_op, ref_val, null_val, m_types.bool_type());
    }

    // Check for string operations - rewrite to native function calls
    if (left_type && left_type->kind == TypeKind::String) {
        ValueId left = gen_expr(binary_expr.left);
        ValueId right = gen_expr(binary_expr.right);

        StringView func_name;
        Type* result_type = nullptr;

        switch (binary_expr.op) {
            case BinaryOp::Add:
                func_name = "str_concat";
                result_type = m_types.string_type();
                break;
            case BinaryOp::Equal:
                func_name = "str_eq";
                result_type = m_types.bool_type();
                break;
            case BinaryOp::NotEqual:
                func_name = "str_ne";
                result_type = m_types.bool_type();
                break;
            default:
                // Unsupported string operation - fall through to regular handling
                // (will likely cause a type error)
                break;
        }

        if (!func_name.empty()) {
            return emit_native(func_name, {left, right}, result_type);
        }
    }

    // Check for struct operator trait dispatch
    if (left_type && left_type->is_struct()) {
        StringView method_name = binary_op_to_trait_method(binary_expr.op);
        if (!method_name.empty()) {
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(left_type, method_name, &found_in);
            if (mi && found_in) {
                // Generate a method call: left.method(right)
                ValueId self_ptr = gen_lvalue_addr(binary_expr.left);
                ValueId right_val = gen_expr(binary_expr.right);

                StringView mangled = mangle_method(found_in->struct_info.name, method_name);
                return emit_call(mangled, alloc_span({self_ptr, right_val}), expr->resolved_type);
            }
        }
    }

    // Short-circuit evaluation for && and ||
    if (binary_expr.op == BinaryOp::And) {
        ValueId left = gen_expr(binary_expr.left);

        IRBlock* right_block = create_block("and.rhs");
        IRBlock* merge_block = create_block("and.end");

        // Merge block receives the result via a block argument
        ValueId result_param = m_current_func->new_value();
        merge_block->params.push_back({result_param, m_types.bool_type(), {}});

        // If left is false, short-circuit: result is false
        ValueId false_val = emit_const_bool(false);
        Span<BlockArgPair> short_circuit_args = alloc_span<BlockArgPair>(1);
        short_circuit_args[0] = {false_val};
        finish_block_branch(left, right_block->id, merge_block->id, {}, short_circuit_args);

        // Evaluate right side, pass result to merge
        set_current_block(right_block);
        ValueId right = gen_expr(binary_expr.right);
        Span<BlockArgPair> right_args = alloc_span<BlockArgPair>(1);
        right_args[0] = {right};
        finish_block_goto(merge_block->id, right_args);

        set_current_block(merge_block);
        return result_param;
    }
    else if (binary_expr.op == BinaryOp::Or) {
        ValueId left = gen_expr(binary_expr.left);

        IRBlock* right_block = create_block("or.rhs");
        IRBlock* merge_block = create_block("or.end");

        // Merge block receives the result via a block argument
        ValueId result_param = m_current_func->new_value();
        merge_block->params.push_back({result_param, m_types.bool_type(), {}});

        // If left is true, short-circuit: result is true
        ValueId true_val = emit_const_bool(true);
        Span<BlockArgPair> short_circuit_args = alloc_span<BlockArgPair>(1);
        short_circuit_args[0] = {true_val};
        finish_block_branch(left, merge_block->id, right_block->id, short_circuit_args, {});

        // Evaluate right side, pass result to merge
        set_current_block(right_block);
        ValueId right = gen_expr(binary_expr.right);
        Span<BlockArgPair> right_args = alloc_span<BlockArgPair>(1);
        right_args[0] = {right};
        finish_block_goto(merge_block->id, right_args);

        set_current_block(merge_block);
        return result_param;
    }

    // Regular binary operations
    ValueId left = gen_expr(binary_expr.left);
    ValueId right = gen_expr(binary_expr.right);
    Type* result_type = expr->resolved_type;
    Type* operand_type = binary_expr.left->resolved_type;

    // Check if it's a comparison or arithmetic operation
    IROp op;
    switch (binary_expr.op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEq:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEq:
            op = get_comparison_op(binary_expr.op, operand_type);
            break;
        default:
            op = get_binary_op(binary_expr.op, operand_type);
            break;
    }

    return emit_binary(op, left, right, result_type);
}

ValueId IRBuilder::gen_ternary_expr(Expr* expr) {
    TernaryExpr& ternary_expr = expr->ternary;

    ValueId cond = gen_expr(ternary_expr.condition);

    IRBlock* then_block = create_block("tern.then");
    IRBlock* else_block = create_block("tern.else");
    IRBlock* merge_block = create_block("tern.end");

    // Merge block takes the ternary result as a phi parameter so both branches
    // can contribute their value. Previously this returned else_val directly,
    // leaving the merge block's register undefined on the then-path and
    // producing garbage at runtime.
    Type* result_type = expr->resolved_type;
    ValueId phi = m_current_func->new_value();
    merge_block->params.push_back({phi, result_type, StringView()});

    finish_block_branch(cond, then_block->id, else_block->id);

    // Then branch
    set_current_block(then_block);
    ValueId then_val = gen_expr(ternary_expr.then_expr);
    {
        Vector<BlockArgPair> args;
        args.push_back({then_val});
        finish_block_goto(merge_block->id, alloc_span(args));
    }

    // Else branch
    set_current_block(else_block);
    ValueId else_val = gen_expr(ternary_expr.else_expr);
    {
        Vector<BlockArgPair> args;
        args.push_back({else_val});
        finish_block_goto(merge_block->id, alloc_span(args));
    }

    set_current_block(merge_block);
    return phi;
}

ValueId IRBuilder::emit_call_resolved(StringView name, Span<ValueId> args, Type* result_type) {
    i32 native_idx = m_registry.get_index(name);
    if (native_idx >= 0) {
        return emit_call_native(name, args, result_type, static_cast<u32>(native_idx));
    }
    return emit_call(name, args, result_type);
}

ValueId IRBuilder::emit_native(StringView name, std::initializer_list<ValueId> args, Type* result_type) {
    i32 native_idx = m_registry.get_index(name);
    if (native_idx < 0) {
        report_error("Internal error: native function not in registry");
        return ValueId::invalid();
    }
    Span<ValueId> arg_span = alloc_span<ValueId>(static_cast<u32>(args.size()));
    u32 i = 0;
    for (ValueId arg : args) {
        arg_span[i++] = arg;
    }
    return emit_call_native(name, arg_span, result_type, static_cast<u32>(native_idx));
}

ValueId IRBuilder::emit_call_indirect(ValueId callee_val, Span<ValueId> args, Type* result_type) {
    IRInst* call_inst = emit_inst(IROp::CallIndirect, result_type);
    if (!call_inst) return ValueId::invalid();
    call_inst->call_indirect.callee = callee_val;
    call_inst->call_indirect.args = args;
    return call_inst->result;
}

Span<ValueId> IRBuilder::prepend_self(ValueId self, Span<ValueId> args, ValueId output_ptr) {
    bool has_output = output_ptr.is_valid();
    u32 extra = has_output ? 2 : 1;
    Span<ValueId> result = alloc_span<ValueId>(static_cast<u32>(args.size()) + extra);
    result[0] = self;
    for (u32 i = 0; i < args.size(); i++) {
        result[i + 1] = args[i];
    }
    if (has_output) {
        result[args.size() + 1] = output_ptr;
    }
    return result;
}

i32 IRBuilder::find_method_fn_index(Type* struct_type, StringView method_name) {
    if (!m_module) return -1;
    Type* found_in = nullptr;
    const MethodInfo* method_info = m_types.lookup_method(struct_type, method_name, &found_in);
    if (!method_info || !found_in) return -1;
    StringView mangled = mangle_method(found_in->struct_info.name, method_name);
    for (u32 fi = 0; fi < m_module->functions.size(); fi++) {
        if (m_module->functions[fi]->name == mangled) {
            return static_cast<i32>(fi);
        }
    }
    return -1;
}

ValueId IRBuilder::gen_list_constructor(Expr* expr) {
    // List<T>() / List<T>(cap): two-step allocate+init matching struct constructors.
    //   1. Call alloc native (element layout args) to get the list pointer
    //   2. Call the constructor method with [self, user_args...]
    CallExpr& call_expr = expr->call;
    Type* list_type = call_expr.callee->resolved_type;

    // Generate user arguments first
    u32 user_argc = static_cast<u32>(call_expr.arguments.size());
    Span<ValueId> user_args = alloc_span<ValueId>(user_argc);
    for (u32 i = 0; i < user_argc; i++) {
        user_args[i] = gen_expr(call_expr.arguments[i].expr);
    }

    // Step 1: Allocate empty list with element_slot_count and element_is_inline args
    StringView alloc_name = list_type->list_info.alloc_native_name;
    Type* elem_type = list_type->list_info.element_type;
    u32 esc = get_type_slot_count(elem_type);
    bool is_inline = !elem_type->is_struct();
    ValueId esc_val = emit_const_int(static_cast<i64>(esc), m_types.i32_type());
    ValueId inline_val = emit_const_int(is_inline ? 1 : 0, m_types.i32_type());
    ValueId list_ptr = emit_native(alloc_name, {esc_val, inline_val}, expr->resolved_type);

    // Step 2: Call constructor method with [self, user_args...]
    StringView ctor_name = call_expr.mangled_name;  // "List$$new"
    i32 ctor_idx = m_registry.get_index(ctor_name);
    Span<ValueId> ctor_args = prepend_self(list_ptr, user_args);
    emit_call_native(ctor_name, ctor_args, m_types.void_type(), static_cast<u32>(ctor_idx));

    // If elements are counted borrows (`ref T`), tag the list so `.copy()`
    // RefIncs each element (mirrors the Map<_, ref V> value_is_ref tag above;
    // push acquires and destroy RefDecs are already compiler-emitted).
    if (elem_type && elem_type->kind == TypeKind::Ref) {
        StringView mark_name("__list_mark_ref_elements", 24);
        i32 mark_idx = m_registry.get_index(mark_name);
        if (mark_idx >= 0) {
            emit_call_native(mark_name, alloc_span({list_ptr}), m_types.void_type(),
                             static_cast<u32>(mark_idx));
        }
    }

    return list_ptr;
}

ValueId IRBuilder::gen_map_constructor(Expr* expr) {
    // Map<K,V>() / Map<K,V>(cap): like the List constructor but injects a hidden
    // key_kind argument, and for struct keys passes the user's hash/eq fn indices.
    CallExpr& call_expr = expr->call;
    Type* map_resolved_type = call_expr.callee->resolved_type;

    // Generate user arguments first (0 or 1 capacity arg)
    u32 user_argc = static_cast<u32>(call_expr.arguments.size());
    Span<ValueId> user_args = alloc_span<ValueId>(user_argc);
    for (u32 i = 0; i < user_argc; i++) {
        user_args[i] = gen_expr(call_expr.arguments[i].expr);
    }

    // Step 1: Allocate empty map with key/value layout. Both keys and values
    // support variable slot counts; for primitive keys the layout is 2-slot inline
    // (the u64 register packs the value), for struct keys the layout matches the
    // struct's slot count and the runtime expects a pointer to the bytes.
    StringView alloc_name = map_resolved_type->map_info.alloc_native_name;
    Type* key_type = map_resolved_type->map_info.key_type;
    Type* value_type = map_resolved_type->map_info.value_type;
    u32 ksc = key_type->is_struct() ? get_type_slot_count(key_type) : 2u;
    bool key_is_inline = !key_type->is_struct();
    u32 vsc = get_type_slot_count(value_type);
    bool value_is_inline = !value_type->is_struct();

    // For struct keys, look up the user's hash()/eq() methods and pass their bytecode
    // indices to the runtime (called via call_user_function during bucket probing).
    // -1 means "no custom impl, use bytewise dispatch". Custom dispatch fires only
    // when the struct EXPLICITLY implements both Hash and Eq via `for Hash` / `for Eq`
    // — just defining hash()/eq() is not enough, matching Rust's HashMap requiring
    // `impl Hash` + `impl Eq`.
    i32 hash_fn_index = -1;
    i32 eq_fn_index = -1;
    if (key_type->is_struct() && m_module) {
        Type* hash_trait = m_type_env.hash_type();
        Type* eq_trait = m_type_env.eq_type();
        if (hash_trait && m_types.implements_trait(key_type, hash_trait)) {
            hash_fn_index = find_method_fn_index(key_type, "hash"_sv);
        }
        if (eq_trait && m_types.implements_trait(key_type, eq_trait)) {
            eq_fn_index = find_method_fn_index(key_type, "eq"_sv);
        }
    }

    ValueId ksc_val = emit_const_int(static_cast<i64>(ksc), m_types.i32_type());
    ValueId kii_val = emit_const_int(key_is_inline ? 1 : 0, m_types.i32_type());
    ValueId vsc_val = emit_const_int(static_cast<i64>(vsc), m_types.i32_type());
    ValueId vii_val = emit_const_int(value_is_inline ? 1 : 0, m_types.i32_type());
    ValueId hash_val = emit_const_int(static_cast<i64>(hash_fn_index), m_types.i32_type());
    ValueId eq_val = emit_const_int(static_cast<i64>(eq_fn_index), m_types.i32_type());
    ValueId map_ptr = emit_native(alloc_name,
                                  {ksc_val, kii_val, vsc_val, vii_val, hash_val, eq_val},
                                  expr->resolved_type);

    // Step 2: Call constructor with [self, key_kind, user_args...]. Determine
    // MapKeyKind from the key type.
    i32 key_kind_val = static_cast<i32>(MapKeyKind::Integer);  // default
    if (key_type->kind == TypeKind::F32) {
        key_kind_val = static_cast<i32>(MapKeyKind::Float32);
    } else if (key_type->kind == TypeKind::F64) {
        key_kind_val = static_cast<i32>(MapKeyKind::Float64);
    } else if (key_type->kind == TypeKind::String) {
        key_kind_val = static_cast<i32>(MapKeyKind::String);
    } else if (key_type->is_struct()) {
        key_kind_val = static_cast<i32>(MapKeyKind::Struct);
    }
    ValueId key_kind_const = emit_const_int(static_cast<i64>(key_kind_val), m_types.i32_type());

    StringView ctor_name = call_expr.mangled_name;  // "Map$$new"
    i32 ctor_idx = m_registry.get_index(ctor_name);
    // Constructor args: [self, key_kind, optional_capacity]
    Span<ValueId> ctor_args = alloc_span<ValueId>(user_argc + 2);
    ctor_args[0] = map_ptr;           // self
    ctor_args[1] = key_kind_const;    // key_kind (hidden)
    for (u32 i = 0; i < user_argc; i++) {
        ctor_args[i + 2] = user_args[i];
    }
    emit_call_native(ctor_name, ctor_args, m_types.void_type(), static_cast<u32>(ctor_idx));

    // If values are counted borrows (`ref V`), tag the map so insert RefIncs and
    // remove/clear/destroy RefDec each value (lifetimes.md "Applying the model").
    if (value_type && value_type->kind == TypeKind::Ref) {
        StringView mark_name("__map_mark_ref_values", 21);
        i32 mark_idx = m_registry.get_index(mark_name);
        if (mark_idx >= 0) {
            emit_call_native(mark_name, alloc_span({map_ptr}), m_types.void_type(),
                             static_cast<u32>(mark_idx));
        }
    }

    return map_ptr;
}

Span<ValueId> IRBuilder::lower_simple_args(Span<CallArg> arguments) {
    Span<ValueId> args = alloc_span<ValueId>(static_cast<u32>(arguments.size()));
    for (u32 i = 0; i < arguments.size(); i++) {
        CallArg& arg = arguments[i];
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue for out/inout args
            args[i] = gen_lvalue_addr(arg.expr);
            continue;
        }
        args[i] = gen_expr(arg.expr);
        if (arg.expr->resolved_type && arg.expr->resolved_type->noncopyable()) {
            // Ownership transfers to the callee: consume a temp (the Nullify
            // ends its cleanup-record scope so it isn't freed a second time)
            // and null a moved-out source field in its root struct.
            consume_temp_noncopyable(args[i]);
            nullify_moved_field_source(arg.expr);
        }
    }
    return args;
}

void IRBuilder::mark_simple_args_moved(Span<CallArg> arguments) {
    for (u32 i = 0; i < arguments.size(); i++) {
        const CallArg& arg = arguments[i];
        if (arg.modifier != ParamModifier::None) continue;
        if (arg.expr->kind != AstKind::ExprIdentifier) continue;
        Type* arg_type = arg.expr->resolved_type;
        if (!arg_type || arg_type->is_copy()) continue;
        mark_moved_from(arg.expr->identifier.name);
    }
}

IRBuilder::CallLowering IRBuilder::lower_call_args(Expr* expr) {
    CallExpr& call_expr = expr->call;
    CallLowering lowered;

    // Callee returning a large struct gets a hidden output pointer (stack slot).
    Type* callee_return_type = expr->resolved_type;
    lowered.returns_large_struct = callee_return_type &&
        callee_return_type->is_struct() &&
        callee_return_type->struct_info.slot_count > 4;
    if (lowered.returns_large_struct) {
        lowered.output_ptr = emit_stack_alloc(callee_return_type->struct_info.slot_count,
                                              callee_return_type);
    }

    // Evaluate arguments - for out/inout args, pass address instead of value
    Span<ValueId> args = alloc_span<ValueId>(call_expr.arguments.size());
    for (u32 i = 0; i < call_expr.arguments.size(); i++) {
        CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue
            args[i] = gen_lvalue_addr(arg.expr);

            // Track primitive inout/out identifiers for post-call reload. Structs are
            // modified in place through the pointer, so they need no reload.
            if (arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out) {
                if (arg.expr->kind == AstKind::ExprIdentifier && !m_param_is_ptr.count(arg.expr->identifier.name)) {
                    Type* type = arg.expr->resolved_type;
                    if (type && type->is_struct()) {
                        continue;
                    }
                    // Structs skipped above; get_type_slot_count gives the correct width
                    // for every remaining type (weak=4, uniq/ref/list/map/string/fn=2).
                    u32 slot_count = slot_count_or_1(type);
                    lowered.inout_args.push_back({arg.expr->identifier.name, args[i], type, slot_count});
                }
            }
        } else {
            args[i] = gen_expr(arg.expr);

            // Consume noncopyable temporaries (ownership transfers to callee).
            // Nullify is a compile-time annotation — it ends the cleanup record
            // scope so exception cleanup skips this value after the transfer.
            // Exception: a `Map<_, noncopyable V>.insert(k, v)` defers its value-arg
            // consume to gen_call_member (after the insert), so the contains-guard
            // branch can't strand the value-Nullify before the insert (step 4).
            bool defer_map_insert_value =
                i == 1 && is_map_insert_noncopyable_value(call_expr);
            if (!defer_map_insert_value &&
                arg.expr->resolved_type && arg.expr->resolved_type->noncopyable()) {
                consume_temp_noncopyable(args[i]);
                // `f(o.field)`: null the moved-out field in the root (args[i]
                // already read its value above) so the root's destructor no-ops it.
                nullify_moved_field_source(arg.expr);
            }

            Type* callee_func_type = call_expr.callee->resolved_type;
            if (callee_func_type) callee_func_type = callee_func_type->base_type();

            // Passing a bare `self` to a `ref` OR `weak` parameter is a promotion:
            // a `ref` param RefIncs at entry and a `weak` param snapshots the
            // generation — both read the receiver's object header — so heap-gate
            // the RAW self pointer here (before the call, and crucially before the
            // maybe_wrap_weak below turns it into a WeakCreate value). A stack
            // receiver traps rather than reading a bogus header (lifetimes.md
            // "Promotion"). The param index accounts for an implicit `self` on
            // method callees; uncertain callee shapes return -1 and are skipped.
            if (is_bare_self(arg.expr) && callee_func_type && callee_func_type->is_function()) {
                i32 off = self_pass_param_offset(call_expr);
                Span<Type*> ptypes = callee_func_type->func_info.param_types;
                if (off >= 0) {
                    u32 pidx = i + static_cast<u32>(off);
                    if (pidx < ptypes.size() && ptypes[pidx] &&
                        (ptypes[pidx]->kind == TypeKind::Ref ||
                         ptypes[pidx]->kind == TypeKind::Weak)) {
                        emit_assert_heap(args[i]);
                    }
                }
            }

            // Wrap uniq/ref → weak conversion for call arguments (after the gate
            // above, so the gate sees the raw pointer, not the wrapped weak).
            if (callee_func_type && callee_func_type->is_function() &&
                i < callee_func_type->func_info.param_types.size()) {
                Type* param_type = callee_func_type->func_info.param_types[i];
                args[i] = maybe_wrap_weak(args[i], arg.expr->resolved_type, param_type);
            }
        }
    }
    lowered.args = args;

    // For large struct returns, append the output pointer to arguments
    if (lowered.returns_large_struct) {
        Span<ValueId> final_args = alloc_span<ValueId>(args.size() + 1);
        for (u32 i = 0; i < args.size(); i++) {
            final_args[i] = args[i];
        }
        final_args[args.size()] = lowered.output_ptr;
        lowered.final_args = final_args;
    } else {
        lowered.final_args = args;
    }

    return lowered;
}

ValueId IRBuilder::gen_call_direct(Expr* expr, const CallLowering& lowered) {
    CallExpr& call_expr = expr->call;
    Span<ValueId> final_args = lowered.final_args;

    // Original (unmangled) callee name from source. For generic calls this is the
    // template name ("helper"); the symbol table is keyed by the template name, not
    // the monomorphized name ("helper$i32"), so we look up via orig_name.
    StringView orig_name = call_expr.callee->identifier.name;
    // Use the mangled name for generic function calls (e.g., "identity$i32")
    StringView func_name = call_expr.mangled_name.size() > 0 ? call_expr.mangled_name : orig_name;
    StringView lookup_name = func_name;

    // Imported functions may have an alias; use the original name for native lookup.
    Symbol* sym = m_symbols.lookup(orig_name);
    if (sym && sym->kind == SymbolKind::ImportedFunction) {
        lookup_name = sym->imported_func.original_name;
    }

    ValueId result;
    // Indirect call: callee is a local holding a closure value (Function-typed).
    // Detect via the local scope map — symbol lookups don't see function-body locals.
    if (LocalVar* lv = find_local(orig_name); lv && lv->type && lv->type->base_type()->is_function()) {
        ValueId closure_val = gen_identifier_expr(call_expr.callee);
        result = emit_call_indirect(closure_val, final_args, expr->resolved_type);
    }
    // Native function
    else if (i32 native_idx = m_registry.get_index(lookup_name); native_idx >= 0) {
        result = emit_call_native(lookup_name, final_args, expr->resolved_type, static_cast<u32>(native_idx));
    } else {
        // Module-scope non-pub functions are mangled at definition (see build_function);
        // calls to them must use the mangled name so they resolve within the same module.
        StringView emit_name = func_name;
        // For generic calls the template lives in the GenericInstantiator rather than
        // the symbol table, so `sym` is null. Look up the template there for its is_pub
        // (build_function uses the same is_pub when emitting the instance).
        bool is_pub = false;
        bool is_function_symbol = false;
        if (sym && sym->kind == SymbolKind::Function) {
            is_function_symbol = true;
            is_pub = sym->is_pub;
        } else if (call_expr.mangled_name.size() > 0 &&
                   m_type_env.generics().is_generic_fun(orig_name)) {
            Decl* template_decl = m_type_env.generics().get_generic_fun_decl(orig_name);
            if (template_decl && template_decl->kind == AstKind::DeclFun) {
                is_function_symbol = true;
                is_pub = template_decl->fun_decl.is_pub;
            }
        }
        if (is_function_symbol && !is_pub
            && orig_name != "main"_sv) {
            emit_name = mangle_module_local(func_name);
        }
        result = emit_call(emit_name, final_args, expr->resolved_type);
    }

    // For large struct returns, the result is the output pointer (already allocated)
    if (lowered.returns_large_struct) {
        result = lowered.output_ptr;
    }
    return result;
}

ValueId IRBuilder::gen_call_member(Expr* expr, const CallLowering& lowered) {
    CallExpr& call_expr = expr->call;
    GetExpr& get_expr = call_expr.callee->get;
    Span<ValueId> args = lowered.args;
    Span<ValueId> final_args = lowered.final_args;

    // Module-qualified call: module.function(). The object's resolved_type is null
    // for module references.
    if (get_expr.object->kind == AstKind::ExprIdentifier && get_expr.object->resolved_type == nullptr) {
        StringView module_name = get_expr.object->identifier.name;
        StringView func_name = get_expr.name;
        // The function name is just the member name for the native registry.
        i32 native_idx = m_registry.get_index(func_name);
        ValueId result;
        if (native_idx >= 0) {
            result = emit_call_native(func_name, final_args, expr->resolved_type, static_cast<u32>(native_idx));
        } else {
            result = emit_call_external(module_name, func_name, final_args, expr->resolved_type);
        }
        if (lowered.returns_large_struct) result = lowered.output_ptr;
        return result;
    }

    // Method call: obj.method(args)
    ValueId obj = gen_expr(get_expr.object);
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    // Calling a method on a `weak T` receiver dereferences it for `self`; validate
    // liveness first (trap on dangling) so the method doesn't run on freed memory.
    if (obj_type && obj_type->kind == TypeKind::Weak) {
        emit_weak_deref_check(obj);
    }

    // Coro method call. Both dispatch dynamically so first-class (erased) Coro<T>
    // values work: a coroutine value is a heap pointer to its state struct whose
    // slot 0 is __resume_idx (the resume function's dispatch index, exactly like a
    // closure env's __call_idx) and slot 1 is __state.
    if (struct_type && struct_type->is_coroutine()) {
        if (get_expr.name == "done"_sv) {
            // Inline: load __state (fixed slot 1) and compare to the done sentinel.
            // Uniform across all coroutines — no dispatch, no $$done function.
            ValueId state = emit_get_field(obj, "__state"_sv, /*slot_offset*/1, /*slot_count*/1,
                                           m_types.i32_type());
            // CORO_STATE_DONE (see coroutine_lowering.cpp): positive sentinel above
            // all yield-point states.
            ValueId sentinel = emit_const_int(0x7FFFFFFF, m_types.i32_type());
            return emit_binary(IROp::EqI, state, sentinel, m_types.bool_type());
        }
        // resume(): CALL_INDIRECT through __resume_idx, the coro value as receiver.
        // (resume takes no explicit args; final_args is empty.)
        return emit_call_indirect(obj, final_args, expr->resolved_type);
    }

    // "Field of function type": obj.callback(args) where callback is a struct field
    // whose type is fun(...) -> R. Checked before method dispatch since a field name
    // could collide with a method name (and we want the field).
    const FieldInfo* fn_field = (struct_type && struct_type->is_struct())
        ? struct_type->struct_info.find_field(get_expr.name) : nullptr;
    if (fn_field && fn_field->type && fn_field->type->base_type()->is_function()) {
        // Read the closure value from the field, then CALL_INDIRECT.
        ValueId closure_val = gen_expr(call_expr.callee);
        return emit_call_indirect(closure_val, final_args, expr->resolved_type);
    }

    // List/Map builtin native method.
    if (struct_type && struct_type->is_container()) {
        // Pushing a `ref` element into a List makes the container hold a counted
        // borrow — increment it. The hold is released when the element leaves
        // (container destroy / overwrite). lifetimes.md "Applying the model".
        if (struct_type->is_list() && get_expr.name == "push"_sv
            && struct_type->list_info.element_type
            && struct_type->list_info.element_type->kind == TypeKind::Ref
            && args.size() >= 1) {
            emit_ref_inc(args[0]);
        }
        // Pushing a `string` element makes the list an owner of it — retain
        // (the list releases each element on destroy via the StrRelease element
        // descriptor). Retaining a fresh temp too keeps the count balanced: the
        // temp's own scope-exit release leaves exactly the list's count (finding 9b).
        if (struct_type->is_list() && get_expr.name == "push"_sv
            && struct_type->list_info.element_type
            && struct_type->list_info.element_type->kind == TypeKind::String
            && args.size() >= 1) {
            emit_str_retain(args[0]);
        }
        // Map insert/remove/clear must destroy noncopyable values that are
        // overwritten / removed / cleared (lifetimes.md "Value lifecycle"). The
        // value_is_ref runtime path already handles `ref` values; this closes the
        // `uniq` (and container / struct-with-dtor) value leak, reusing existing IR
        // ops so both backends get it for free.
        // insert-replace cleanup needs special ordering: its value-arg consume is
        // deferred (lower_call_args skipped it) so we run contains-guard → insert →
        // consume here, keeping them in one block (the contains-guard's branch
        // would otherwise strand the value-Nullify before the insert, storing null;
        // see is_map_insert_noncopyable_value). remove/clear have no value arg, so
        // their cleanup is a plain pre-call destroy.
        if (struct_type->is_map() && is_map_insert_noncopyable_value(call_expr)
            && args.size() >= 2) {
            emit_map_value_delete_if_present(obj, struct_type, args[0]);  // destroy old, if present
            StringView native_name = call_expr.mangled_name;
            i32 native_idx = m_registry.get_index(native_name);
            Span<ValueId> method_args = prepend_self(obj, args);
            ValueId result = emit_call_native(native_name, method_args, expr->resolved_type,
                                              static_cast<u32>(native_idx));
            // Deferred consume — now after the insert, in the merge block.
            consume_temp_noncopyable(args[1]);
            nullify_moved_field_source(call_expr.arguments[1].expr);
            return result;
        }
        if (struct_type->is_map()) {
            if (get_expr.name == "remove"_sv && args.size() >= 1) {
                emit_map_value_delete_if_present(obj, struct_type, args[0]);
            } else if (get_expr.name == "clear"_sv) {
                emit_map_clear_value_cleanup(obj, struct_type);
            }
        }
        StringView native_name = call_expr.mangled_name;
        i32 native_idx = m_registry.get_index(native_name);
        Span<ValueId> method_args = prepend_self(obj, args);
        return emit_call_native(native_name, method_args, expr->resolved_type, static_cast<u32>(native_idx));
    }

    // User struct method. Look up the method in the type hierarchy to find where it's
    // defined, so mangling uses the correct struct name (important for inheritance).
    Type* method_owner = nullptr;
    StringView method_name = get_expr.name;
    if (struct_type && struct_type->is_struct()) {
        lookup_method_in_hierarchy(struct_type, get_expr.name, &method_owner);
        Type* name_type = method_owner ? method_owner : struct_type;
        method_name = mangle_method(name_type->struct_info.name, get_expr.name);
    }

    // Constraint-reference model (lifetimes.md "Counting mechanics"): count a *heap* receiver
    // for the call's duration. Whenever the receiver is statically heap (its type
    // is `uniq`), increment its borrow count across the call so a reentrant free
    // of the receiver — directly or through an alias the callee reaches — traps in
    // object_free instead of dangling. `obj` is already the receiver object's heap
    // data pointer, so this covers every receiver shape uniformly: a bare `uniq`
    // local/param (`c.method()`), a `uniq` field root (`o.inner.method()` — `obj`
    // is the field's stored pointer, so the borrow lands on the receiver Inner
    // object itself, which a `delete o` would also try to free → it traps), and a
    // heap-returning temp (`make().method()` — counted distinctly from the temp's
    // own scope-exit Delete via the pinned copy). A `ref` receiver is already
    // covered by its own count; a container subscript yields `borrowed`→`ref`, so
    // it too falls out here; stack value-struct receivers are second-class
    // (downward-safe) and uncounted. The borrow rides a *pinned* Copy so it owns a
    // distinct SSA value/register: its RefDec + Nullify cleanup can't collide with
    // an owned-local's / temp's own Delete record (which shares the receiver value).
    ValueId recv_borrow = ValueId::invalid();
    BlockId recv_borrow_block = BlockId::invalid();
    if (struct_type && struct_type->is_struct()
        && obj_type && obj_type->kind == TypeKind::Uniq
        && m_current_block) {
        recv_borrow = emit_pinned_copy(obj, obj_type);
        emit_ref_inc(recv_borrow);
        recv_borrow_block = m_current_block->id;
    }

    // [obj] + args, with a trailing output pointer when this returns a large struct
    // (output_ptr is invalid otherwise, so prepend_self appends nothing).
    Span<ValueId> method_args = prepend_self(obj, args, lowered.output_ptr);
    ValueId result = emit_call_resolved(method_name, method_args, expr->resolved_type);

    // Close the receiver borrow: balanced RefDec on the normal path, plus a
    // call-scoped exception cleanup record so a throw out of the call releases
    // the borrow before the owner is unwound. The record is deferred (appended
    // at end_function_body) so it sorts after the owner's Delete record and
    // therefore runs first on the reverse-ordered unwind. The Nullify ends the
    // record's scope at the RefDec; lowering narrows its start to the RefInc.
    if (recv_borrow.is_valid() && m_current_block) {
        emit_ref_dec(recv_borrow);
        emit_nullify(recv_borrow);
        IRCleanupInfo ci;
        ci.value = recv_borrow;
        ci.type = obj_type;
        ci.start_block = recv_borrow_block;
        ci.end_block = m_current_block->id;
        ci.kind = IRCleanupKind::RefDec;
        ci.call_borrow = true;
        m_call_borrow_cleanups.push_back(ci);
    }

    if (lowered.returns_large_struct) result = lowered.output_ptr;
    return result;
}

// True if expr is composed only of identifiers and field accesses, so
// re-evaluating it via gen_expr is side-effect-free and idempotent (no calls or
// index operations whose re-execution could change observable state).
static bool is_pure_field_path(Expr* expr) {
    while (expr) {
        if (expr->kind == AstKind::ExprIdentifier) return true;
        if (expr->kind == AstKind::ExprGet) { expr = expr->get.object; continue; }
        return false;
    }
    return false;
}

// If `lvalue` addresses storage that lives inside a container's element buffer —
// the element itself (`list[i]`) or a field / subfield of an element
// (`list[i].field`, `map[k].a.b`) — return the container object expression so the
// call site can pin it (borrow_count) for the call's duration. A mid-call
// structural mutation (push/pop/insert/remove/clear reallocs or frees the buffer)
// or free of that container would otherwise dangle the interior element address.
// Returns null for any lvalue not rooted in a container subscript (a bare
// identifier or a field chain bottoming out at one — those are handled by
// heap_root_of_lvalue or are stack-frame-rooted). A field chain is walked down
// through ExprGet; the first ExprIndex on a container is the root.
static Expr* container_index_root_of_lvalue(Expr* lvalue) {
    for (Expr* e = lvalue; e; ) {
        if (e->kind == AstKind::ExprIndex) {
            Expr* obj = e->index.object;
            Type* obj_type = obj ? obj->resolved_type : nullptr;
            Type* base = obj_type ? obj_type->base_type() : nullptr;
            return (base && base->is_container()) ? obj : nullptr;
        }
        if (e->kind == AstKind::ExprGet) { e = e->get.object; continue; }
        return nullptr;
    }
    return nullptr;
}

ValueId IRBuilder::heap_root_of_lvalue(Expr* lvalue, Type** out_type) {
    // Only field-access chains point *into* another object's storage. A bare
    // identifier names a slot on the caller's own frame (the slot outlives the
    // call); an index/call base isn't a pure lvalue we can recount. So the only
    // shape with a heap root to count is `object.field`.
    if (!lvalue || lvalue->kind != AstKind::ExprGet) return ValueId::invalid();
    Expr* object = lvalue->get.object;
    if (!object) return ValueId::invalid();
    Type* obj_type = object->resolved_type;

    if (obj_type && obj_type->kind == TypeKind::Uniq) {
        // We dereference `object` (a heap owner) to reach the field, so the field
        // lives in object's pointee — that pointee is the heap root. Re-evaluate
        // it (idempotent for a pure path) to get a countable data pointer.
        if (!is_pure_field_path(object)) return ValueId::invalid();
        if (out_type) *out_type = obj_type;
        return gen_expr(object);
    }
    if (obj_type && (obj_type->kind == TypeKind::Ref || obj_type->kind == TypeKind::Weak)) {
        // A `ref` already holds a count on its pointee for its own lifetime, so
        // the pointee can't be freed mid-call anyway; `weak` field-lvalues don't
        // occur (a weak must be checked before use). Nothing to add.
        return ValueId::invalid();
    }
    // `object` holds the field inline in its own storage (a value struct, on the
    // stack or inline within a heap object). Whatever object itself roots in is
    // the heap root; recurse. A stack identifier root bottoms out at invalid.
    Type* obj_base = obj_type ? obj_type->base_type() : nullptr;
    if (obj_base && obj_base->is_struct()) {
        return heap_root_of_lvalue(object, out_type);
    }
    return ValueId::invalid();
}

void IRBuilder::reload_inout_args(const CallLowering& lowered) {
    // After the call, reload inout variables from their stack addresses.
    for (const InoutArg& ia : lowered.inout_args) {
        ValueId new_val = emit_load_ptr(ia.addr, ia.slot_count, ia.type);
        define_local(ia.name, new_val, ia.type);
    }
}

void IRBuilder::mark_call_args_moved(Expr* expr) {
    CallExpr& call_expr = expr->call;
    // Mark owned args passed to owned params as moved (suppresses scope-exit cleanup;
    // for uniq, mark_moved_from also nulls the register). Skip inout/out: those pass a
    // pointer to the caller's slot, ownership stays with the caller — marking them moved
    // would trip a false use-after-move on the next loop iteration and (for noncopyable
    // types) null out a local the caller still owns.
    Type* callee_func_type = call_expr.callee->resolved_type;
    if (callee_func_type) callee_func_type = callee_func_type->base_type();
    if (!callee_func_type || !callee_func_type->is_function()) return;
    Span<Type*> param_types = callee_func_type->func_info.param_types;
    // Offset user args past the implicit `self` for genuine method callees only.
    // The old blanket "GetExpr callee → 1" misaligned module-qualified calls and
    // field-stored closures (whose function types have no implicit self), so a
    // noncopyable argument was never marked moved — the caller's scope-exit
    // Delete then double-freed the object the callee already owned.
    i32 self_offset = self_pass_param_offset(call_expr);
    if (self_offset < 0) return;
    u32 param_offset = static_cast<u32>(self_offset);
    for (u32 i = 0; i < call_expr.arguments.size() && (i + param_offset) < param_types.size(); i++) {
        const CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::None) continue;
        if (arg.expr->kind != AstKind::ExprIdentifier) continue;
        Type* arg_type = arg.expr->resolved_type;
        Type* param_type = param_types[i + param_offset];
        if (arg_type && arg_type->noncopyable() && param_type && param_type->noncopyable()) {
            // For uniq: null-ify so DEL_OBJ on scope exit is a safe no-op. For value
            // structs: the bitwise copy IS the move, just suppress cleanup.
            mark_moved_from(arg.expr->identifier.name);
        }
    }
}

ValueId IRBuilder::gen_call_expr(Expr* expr) {
    CallExpr& call_expr = expr->call;
    Type* callee_type = call_expr.callee->resolved_type;

    // Type-driven early delegations when the callee is a bare identifier.
    if (call_expr.callee->kind == AstKind::ExprIdentifier && callee_type) {
        if (callee_type->is_primitive() && !callee_type->is_void()) {
            return gen_primitive_cast(expr);     // i32(x), f64(y), ...
        }
        if (callee_type->is_struct()) {
            return gen_constructor_call(expr);   // Foo(...)
        }
        if (callee_type->is_list()) {
            return gen_list_constructor(expr);   // List<T>() / List<T>(cap)
        }
        if (callee_type->is_map()) {
            return gen_map_constructor(expr);    // Map<K,V>() / Map<K,V>(cap)
        }
    }

    // Check if this is a named constructor call: Type.ctor_name(...)
    // The callee is a GetExpr where the object is a type name (not a variable)
    // For named constructors: ge.object is an identifier that resolves to a STRUCT TYPE (not a variable of struct type)
    // This is detected by checking if the identifier matches a type name
    if (call_expr.callee->kind == AstKind::ExprGet) {
        GetExpr& get_expr = call_expr.callee->get;
        if (get_expr.object->kind == AstKind::ExprIdentifier) {
            // Check if this is a type name (constructor) or a variable (method call)
            StringView name = get_expr.object->identifier.name;
            Type* named_type = m_type_env.named_type_by_name(name);
            if (named_type && named_type->is_struct()) {
                // This is a named constructor call: Type.ctor_name(...)
                return gen_constructor_call(expr);
            }
            // Otherwise, it's a method call on a variable - fall through
        }
    }

    // Check if this is a super call: super() / super.ctor_name() / super.method_name()
    if (call_expr.callee->kind == AstKind::ExprSuper) {
        return gen_super_call(expr);
    }

    // Lower arguments once (out/inout addresses, large-struct output pointer, weak
    // wrapping, temp consumption), then dispatch on the callee's shape.
    CallLowering lowered = lower_call_args(expr);

    // Call-site heap-root counting for out/inout arguments (lifetimes.md "Counting mechanics"):
    // an out/inout arg pointing *into* a heap object's storage (`f(inout
    // heap_obj.field)`) borrows that heap root for the call's duration, so a
    // mid-call free of the root — through an alias the callee reaches — traps in
    // object_free instead of dangling the pointer. The borrow rides a pinned copy
    // (distinct SSA identity, like the receiver borrow) and is RefDec'd after the
    // call; its exception record is deferred so it releases before any owner Delete
    // on unwind. Method-receiver borrows are handled separately in gen_call_member.
    // Two flavours, both riding a pinned copy + a deferred call-scoped exception
    // record so they release before any owner Delete on unwind:
    //   - field-rooted lvalue (`f(inout heap_obj.field)`): RefInc the heap root
    //     (free-block), released by RefDec.
    //   - container-index lvalue (`f(inout list[i])`, lifetimes.md "Container element lvalues"): pin the
    //     container (borrow_count), so a mid-call realloc/free of it traps before
    //     the element address can dangle; released by unpin.
    Vector<IRCleanupInfo> call_borrows;
    for (u32 i = 0; i < call_expr.arguments.size() && m_current_block; i++) {
        CallArg& arg = call_expr.arguments[i];
        if (arg.modifier != ParamModifier::Inout && arg.modifier != ParamModifier::Out) continue;

        Type* root_type = nullptr;
        ValueId root = heap_root_of_lvalue(arg.expr, &root_type);
        if (root.is_valid()) {
            ValueId borrow = emit_pinned_copy(root, root_type);
            emit_ref_inc(borrow);
            IRCleanupInfo ci;
            ci.value = borrow;
            ci.type = root_type;
            ci.start_block = m_current_block->id;
            ci.kind = IRCleanupKind::RefDec;
            ci.call_borrow = true;
            call_borrows.push_back(ci);
            continue;
        }

        // Container-element lvalue → pin the container for the call. This covers
        // both the element itself (`inout list[i]`) and a field of an element
        // (`inout list[i].field`, `inout map[k].a.b`): all address storage inside
        // the container's element buffer, so a mid-call realloc/free would dangle
        // the interior pointer. The pin (borrow_count) makes any structural
        // mutation or free of the container trap for the call's duration.
        if (Expr* obj = container_index_root_of_lvalue(arg.expr)) {
            Type* obj_type = obj->resolved_type;
            ValueId container = gen_expr(obj);
            ValueId pin = emit_pinned_copy(container, obj_type);
            emit_container_pin(pin);
            IRCleanupInfo ci;
            ci.value = pin;
            ci.type = obj_type;
            ci.start_block = m_current_block->id;
            ci.kind = IRCleanupKind::Unpin;
            ci.call_borrow = true;
            call_borrows.push_back(ci);
        }
    }

    ValueId result;
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        result = gen_call_direct(expr, lowered);
    }
    else if (call_expr.callee->kind == AstKind::ExprGet) {
        result = gen_call_member(expr, lowered);
    }
    else if (callee_type && callee_type->base_type()->is_function()) {
        // General indirect call: callee is some Function-typed expression (call
        // result, index, field access, ...) — including a borrowed `ref fun`,
        // which shares the env-pointer representation. Evaluate and CALL_INDIRECT.
        ValueId closure_val = gen_expr(call_expr.callee);
        result = emit_call_indirect(closure_val, lowered.final_args, expr->resolved_type);
    }
    else {
        report_error("Internal error: unhandled call expression kind");
        return ValueId::invalid();
    }

    // If a dispatch helper bailed because the block was terminated, stop here to
    // match the original early-return (no inout reload / move-marking on a dead block).
    if (!m_current_block) return result;

    // Close the call-site borrows: balanced release on the normal path (RefDec or
    // unpin) plus a deferred call-scoped exception record. Lowering narrows each to
    // [open, Nullify) so it covers exactly the call window.
    for (IRCleanupInfo& ci : call_borrows) {
        if (ci.kind == IRCleanupKind::Unpin) emit_container_unpin(ci.value);
        else emit_ref_dec(ci.value);
        emit_nullify(ci.value);
        ci.end_block = m_current_block->id;
        m_call_borrow_cleanups.push_back(ci);
    }

    reload_inout_args(lowered);
    mark_call_args_moved(expr);
    return result;
}

ValueId IRBuilder::gen_index_expr(Expr* expr) {
    IndexExpr& index_expr = expr->index;

    Type* obj_type = index_expr.object->resolved_type;
    Type* base_type = obj_type ? obj_type->base_type() : nullptr;

    // Struct indexing: dispatch to "index" method
    if (base_type && base_type->is_struct()) {
        StringView method_name("index", 5);
        Type* found_in = nullptr;
        const MethodInfo* method_info = lookup_method_in_hierarchy(base_type, method_name, &found_in);
        if (method_info && found_in) {
            ValueId self_ptr = gen_lvalue_addr(index_expr.object);
            ValueId index_val = gen_expr(index_expr.index);
            StringView mangled = mangle_method(found_in->struct_info.name, method_name);
            return emit_call(mangled, alloc_span({self_ptr, index_val}), expr->resolved_type);
        }
    }

    // List/Map indexing: emit IndexGet IR op
    if (base_type && base_type->is_container()) {
        ValueId obj = gen_expr(index_expr.object);
        ValueId index_val = gen_expr(index_expr.index);
        ContainerKind kind = base_type->is_list() ? ContainerKind::List : ContainerKind::Map;
        return emit_index_get(obj, index_val, kind, expr->resolved_type);
    }

    report_error("Internal error: index operation not supported");
    return ValueId::invalid();
}

void IRBuilder::emit_weak_deref_check(ValueId weak_val) {
    // A `weak T` is a {ptr, generation} snapshot; reading through it after the
    // referent was freed (tombstoned) or its slot recycled would be a silent
    // stale read. WeakCheck returns false in that case; trap on it (there is no
    // null to yield for a field value — the model is "asserts when a dangling
    // reference is used"). Mirrors the variant-field discriminant guard.
    ValueId valid = emit_unary(IROp::WeakCheck, weak_val, m_types.bool_type());
    IRBlock* ok_block = create_block("weak_ok");
    IRBlock* dead_block = create_block("weak_dead");
    finish_block_branch(valid, ok_block->id, dead_block->id);
    set_current_block(dead_block);
    finish_block_unreachable();
    set_current_block(ok_block);
}

ValueId IRBuilder::gen_get_expr(Expr* expr) {
    GetExpr& get_expr = expr->get;

    ValueId obj = gen_expr(get_expr.object);

    // Get the struct type from the object
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    // Dereferencing a `weak T` must validate liveness first (trap on dangling).
    if (obj_type && obj_type->kind == TypeKind::Weak) {
        emit_weak_deref_check(obj);
    }

    FieldAccess access = resolve_field_access(struct_type, get_expr.name);

    // Variant field: emit the runtime discriminant check before touching it.
    emit_variant_guard(obj, access, "variant_check");

    // If the field is a struct type, we need to compute its address (pointer)
    // instead of loading its value, since nested struct access needs the address.
    if (access.field_type && access.field_type->is_struct()) {
        return emit_get_field_addr(obj, get_expr.name, access.slot_offset, expr->resolved_type);
    }

    return emit_get_field(obj, get_expr.name, access.slot_offset, access.slot_count,
                          expr->resolved_type);
}

IRBuilder::FieldAccess IRBuilder::resolve_field_access(Type* struct_type, StringView name) {
    FieldAccess access;
    if (!struct_type || !struct_type->is_struct()) return access;

    if (const FieldInfo* field_info = struct_type->struct_info.find_field(name)) {
        access.slot_offset = field_info->slot_offset;
        access.slot_count = field_info->slot_count;
        access.field_type = field_info->type;
        return access;
    }

    // Check for variant field in when clauses. The actual offset is
    // union_slot_offset + the variant field's offset within the union.
    const VariantFieldInfo* variant_field_info = struct_type->struct_info.find_variant_field(
        name, &access.when_clause, &access.variant);
    if (variant_field_info) {
        access.is_variant_field = true;
        access.slot_offset = access.when_clause->union_slot_offset + variant_field_info->slot_offset;
        access.slot_count = variant_field_info->slot_count;
        access.field_type = variant_field_info->type;
    }
    return access;
}

void IRBuilder::emit_variant_guard(ValueId obj, const FieldAccess& access, const char* label) {
    if (!access.is_variant_field || !access.when_clause || !access.variant) return;

    // Load the discriminant and compare against this variant's value.
    ValueId disc = emit_get_field(obj, access.when_clause->discriminant_name,
                                  access.when_clause->discriminant_slot_offset, 1,
                                  access.when_clause->discriminant_type);
    ValueId expected = emit_const_int(access.variant->discriminant_value,
                                      access.when_clause->discriminant_type);
    ValueId matches = emit_binary(IROp::EqI, disc, expected, m_types.bool_type());

    IRBlock* pass_block = create_block(intern_concat(label, "_pass"));
    IRBlock* fail_block = create_block(intern_concat(label, "_fail"));
    finish_block_branch(matches, pass_block->id, fail_block->id);

    // In fail block: emit unreachable (will lower to TRAP)
    set_current_block(fail_block);
    finish_block_unreachable();

    // Continue in pass block
    set_current_block(pass_block);
}

ValueId IRBuilder::gen_assign_expr(Expr* expr) {
    AssignExpr& assign_expr = expr->assign;

    // Evaluate the RHS, then fold in any compound operator (`+=`, `-=`, ...).
    ValueId value = gen_expr(assign_expr.value);
    if (assign_expr.op != AssignOp::Assign) {
        bool handled = false;
        ValueId combined = gen_compound_assign(expr, value, handled);
        if (handled) return combined;  // struct trait dispatch did the whole assignment
        value = combined;
    }

    // Dispatch on the assignment target.
    switch (assign_expr.target->kind) {
        case AstKind::ExprIdentifier: return gen_assign_local(expr, value);
        case AstKind::ExprGet:        return gen_assign_field(expr, value);
        case AstKind::ExprIndex:      return gen_assign_index(expr, value);
        default:                      return value;
    }
}

ValueId IRBuilder::gen_compound_assign(Expr* expr, ValueId rhs, bool& handled) {
    AssignExpr& assign_expr = expr->assign;
    handled = false;
    Type* type = assign_expr.target->resolved_type;

    // Struct compound assignment trait dispatch (e.g. `a += b` -> Add's add_assign).
    if (type && type->is_struct()) {
        const char* method_name_str = assign_op_to_trait_method(assign_expr.op);
        if (method_name_str) {
            StringView method_name(method_name_str, static_cast<u32>(strlen(method_name_str)));
            Type* found_in = nullptr;
            const MethodInfo* mi = lookup_method_in_hierarchy(type, method_name, &found_in);
            if (mi && found_in) {
                ValueId self_ptr = gen_lvalue_addr(assign_expr.target);
                StringView mangled = mangle_method(found_in->struct_info.name, method_name);
                handled = true;
                return emit_call(mangled, alloc_span({self_ptr, rhs}), m_types.void_type());
            }
        }
    }

    // Primitive compound assignment: fold the target's current value with the RHS.
    ValueId old_val = gen_expr(assign_expr.target);

    // Unsigned div/mod/shr diverge for u32/u64 targets (see get_binary_op).
    bool is_unsigned = type && (type->kind == TypeKind::U32 || type->kind == TypeKind::U64);
    IROp op;
    switch (assign_expr.op) {
        case AssignOp::AddAssign:
            op = type->is_float() ? IROp::AddF : IROp::AddI;
            break;
        case AssignOp::SubAssign:
            op = type->is_float() ? IROp::SubF : IROp::SubI;
            break;
        case AssignOp::MulAssign:
            op = type->is_float() ? IROp::MulF : IROp::MulI;
            break;
        case AssignOp::DivAssign:
            op = type->is_float() ? IROp::DivF : (is_unsigned ? IROp::DivU : IROp::DivI);
            break;
        case AssignOp::ModAssign:
            op = is_unsigned ? IROp::ModU : IROp::ModI;
            break;
        case AssignOp::BitAndAssign:
            op = IROp::BitAnd;
            break;
        case AssignOp::BitOrAssign:
            op = IROp::BitOr;
            break;
        case AssignOp::BitXorAssign:
            op = IROp::BitXor;
            break;
        case AssignOp::ShlAssign:
            op = IROp::Shl;
            break;
        case AssignOp::ShrAssign:
            op = is_unsigned ? IROp::UShr : IROp::Shr;
            break;
        default:
            op = IROp::Copy;
            break;
    }

    // Narrow-integer targets auto-narrow: `x op= y`  ==>  `x = T(x op y)`. The fold is
    // computed at i32 (matching the promotion in get_binary_result_type), then truncated
    // back to the narrow type via IROp::Cast (lowers to TRUNC_S/TRUNC_U). Plain assignment
    // still requires an explicit cast; compound assignment is the ergonomic exception.
    if (type && type->is_narrow_integer()) {
        Type* i32_type = m_types.i32_type();
        ValueId wide = emit_binary(op, old_val, rhs, i32_type);
        IRInst* cast = emit_inst(IROp::Cast, type);
        if (cast) {
            cast->cast.source = wide;
            cast->cast.source_type = i32_type;
            return cast->result;
        }
        return wide;
    }

    return emit_binary(op, old_val, rhs, type);
}

ValueId IRBuilder::gen_assign_local(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    StringView name = assign_expr.target->identifier.name;

    // If this is a pointer parameter (out/inout), store through the pointer
    if (m_param_is_ptr.count(name)) {
        ValueId ptr = lookup_local(name);
        Type* type = expr->resolved_type;
        // get_type_slot_count handles structs (struct_info.slot_count) and every other
        // width uniformly. The old inline check omitted list/map/string/weak, under-counting
        // their slots when stored through an out/inout pointer.
        u32 slot_count = slot_count_or_1(type);

        // Reassigning an owning out/inout pointer must free the value it
        // currently points at, or the overwritten object leaks — the same
        // old-value destroy gen_assign_field does for `uniq` fields. Pointer-
        // valued owners (uniq/list/map/coro/fun) hold the owned pointer in the
        // slot, so load it and emit a typed Delete; DEL_OBJ on null is a safe
        // no-op, so a zero-initialized (uninitialized `out`) slot is harmless.
        // Value-struct inout reassignment is a separate, rarer path, left as-is.
        if (holds_owning_pointer(type)) {
            ValueId old = emit_load_ptr(ptr, slot_count, type);
            emit_delete(old, type);
        }

        ValueId result = emit_store_ptr(ptr, value, slot_count, type);

        // Consume the RHS temporary so it isn't double-owned (stored through the
        // pointer AND deleted at scope exit), and move-from an identifier RHS.
        // Mirrors gen_assign_field.
        if (type && type->noncopyable()) {
            consume_temp_noncopyable(value);
            if (assign_expr.value->kind == AstKind::ExprIdentifier) {
                Type* value_type = assign_expr.value->resolved_type;
                if (value_type && value_type->noncopyable()) {
                    mark_moved_from(assign_expr.value->identifier.name);
                }
            }
        }
        return result;
    }

    // Module-level global write: store through the global's address, with the
    // same old-value destroy + temp-consume as field/inout assignment. A local
    // of the same name shadows the global (find_local would resolve it), so only
    // reach here for a true global.
    if (!find_local(name)) {
        auto git = m_global_indices.find(name);
        if (git != m_global_indices.end()) {
            Type* gtype = m_module->globals[git->second].type;
            u32 gslots = m_module->globals[git->second].slot_count;
            u32 goffset = m_module->globals[git->second].slot_offset;
            ValueId addr = emit_global_addr(goffset, gtype);
            // Destroy the overwritten value (mirrors gen_assign_field).
            if (gtype && gtype->is_struct() && gtype->noncopyable()) {
                emit_delete(addr, gtype);
            } else if (holds_owning_pointer(gtype)) {
                ValueId old = emit_load_ptr(addr, gslots, gtype);
                emit_delete(old, gtype);
            }
            value = maybe_wrap_weak(value, assign_expr.value->resolved_type, gtype, assign_expr.value);
            ValueId result;
            if (gtype && gtype->is_struct()) {
                emit_struct_copy(addr, value, gslots);
                result = value;
            } else {
                result = emit_store_ptr(addr, value, gslots, gtype);
            }
            if (gtype && gtype->noncopyable()) {
                consume_temp_noncopyable(value);
                if (assign_expr.value->kind == AstKind::ExprIdentifier) {
                    Type* value_type = assign_expr.value->resolved_type;
                    if (value_type && value_type->noncopyable()) {
                        mark_moved_from(assign_expr.value->identifier.name);
                    }
                }
            }
            return result;
        }
    }

    // Auto-destroy old owned value before reassignment
    Type* target_type = assign_expr.target->resolved_type;

    // Ref reassignment (`r = other`): release the old borrow and acquire the new
    // one so the count stays balanced — the variable now borrows a different
    // object. Emitted before define_local so lookup_local still returns the old
    // value. Applies to ref locals and ref params uniformly. A call result
    // already carries a handed-off count, so it's adopted (no inc); any other
    // source is a fresh borrow (inc) — mirrors gen_var_decl.
    if (target_type && target_type->kind == TypeKind::Ref) {
        emit_ref_dec(lookup_local(name));
        if (!is_ref_handoff_source(assign_expr.value)) {
            // `r = self` is a promotion: the inc is heap-gated.
            emit_ref_borrow_inc(value, assign_expr.value);
        }
    }

    // String reassignment (`s = other`): release the old string (frees at zero),
    // then adopt a fresh producer temp or retain an existing owner (finding 9b).
    // Emitted before define_local so lookup_local still returns the old value.
    if (target_type && target_type->kind == TypeKind::String) {
        emit_str_release(lookup_local(name));
        consume_or_retain_string(value, target_type, /*adopted_by_variable=*/true);
    }

    if (target_type && target_type->noncopyable()) {
        OwnedLocalInfo* owned_info = m_ownership.find_by_name(name);
        if (owned_info && !owned_info->is_moved) {
            emit_implicit_destroy(*owned_info);
            owned_info->is_moved = false;  // Reset — new value is now live
        } else if (owned_info && owned_info->is_moved) {
            // Variable was moved but now being reassigned — make it live again
            owned_info->is_moved = false;
        }
    }

    // Wrap uniq/ref → weak conversion for local assignment
    value = maybe_wrap_weak(value, assign_expr.value->resolved_type, assign_expr.target->resolved_type,
                            assign_expr.value);

    // For copyable struct rvalues that alias source storage, allocate fresh
    // storage and emit a StructCopy — mirrors the gen_var_decl fix. Struct
    // literals and calls already produce fresh storage, so skip them.
    // Only applies to simple `=`; compound ops on structs dispatch through
    // trait methods above, and primitive compound ops don't land here with
    // a struct target.
    bool value_is_fresh = assign_expr.value->kind == AstKind::ExprStructLiteral ||
                          assign_expr.value->kind == AstKind::ExprCall;
    if (assign_expr.op == AssignOp::Assign && target_type && target_type->is_struct()
        && target_type->is_copy() && !value_is_fresh) {
        u32 slot_count = target_type->struct_info.slot_count;
        ValueId fresh = emit_stack_alloc(slot_count, target_type);
        emit_struct_copy(fresh, value, slot_count);
        value = fresh;
    }

    // Normal variable assignment - in SSA, we create a new value
    define_local(name, value, expr->resolved_type);

    // If the RHS was a noncopyable temporary (e.g. `uniq T()`), transfer
    // its ownership to the target variable. Without this, the temp's
    // cleanup record at the current scope depth races with the
    // variable's cleanup record at the variable's (outer) scope depth —
    // harmless when register allocation aliases them and tombstoning
    // absorbs the double-delete, but catastrophic inside nested scopes
    // (e.g. a catch body) where the temp's scope pops before the
    // variable's value is observed, leaving the variable pointing at
    // freed memory. Matches the consume_temp_noncopyable(value, true)
    // call in gen_var_decl.
    if (assign_expr.target->resolved_type &&
        assign_expr.target->resolved_type->noncopyable()) {
        consume_temp_noncopyable(value, true);
    }

    // Move semantics: if value is a noncopyable identifier, mark source as moved.
    // Unlike field assignment, we pass nullify_record=false: the target variable now
    // shares the same SSA value/register as the source, so emitting a Nullify on that
    // register would corrupt the target. The SSA null-out of the source still happens.
    if (assign_expr.value->kind == AstKind::ExprIdentifier) {
        Type* value_type = assign_expr.value->resolved_type;
        if (value_type && value_type->noncopyable()) {
            mark_moved_from(assign_expr.value->identifier.name, /*null_ssa=*/true,
                            /*nullify_record=*/false);
        }
    }
    // `y = o.field`: null the moved-out source field in its root.
    if (assign_expr.value->resolved_type && assign_expr.value->resolved_type->noncopyable()) {
        nullify_moved_field_source(assign_expr.value);
    }

    return value;
}

ValueId IRBuilder::gen_assign_field(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    GetExpr& get_expr = assign_expr.target->get;
    ValueId obj = gen_expr(get_expr.object);

    // Get the struct type from the object
    Type* obj_type = get_expr.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    // Writing through a `weak T` must validate liveness first (trap on dangling),
    // else the store lands in freed/recycled memory.
    if (obj_type && obj_type->kind == TypeKind::Weak) {
        emit_weak_deref_check(obj);
    }

    FieldAccess access = resolve_field_access(struct_type, get_expr.name);
    u32 slot_offset = access.slot_offset;
    u32 slot_count = access.slot_count;
    Type* field_type = access.field_type;

    // Variant field: emit the runtime discriminant check before storing.
    emit_variant_guard(obj, access, "variant_set");

    // Reassigning a tagged-union discriminant abandons the current variant: drop
    // its owned fields and clear the union storage before the new discriminant is
    // stored. Only a *regular* field can be a discriminant (a variant-field write
    // took the branch above), so check the struct's when-clauses by name.
    if (!access.is_variant_field && struct_type && struct_type->is_struct()) {
        for (const auto& clause : struct_type->struct_info.when_clauses) {
            if (clause.discriminant_name == get_expr.name) {
                emit_discriminant_reassign_cleanup(obj, clause, value);
                break;
            }
        }
    }

    // Overwriting a `ref` field rebalances the borrow count: release the old
    // borrow, acquire the new (lifetimes.md "Value lifecycle" — mirrors the List
    // ref-element overwrite in gen_assign_index). A freshly default-constructed
    // field holds null, and ref_dec(null) is a no-op, so this is safe even for the
    // first assignment.
    if (field_type && field_type->kind == TypeKind::Ref) {
        ValueId old = emit_get_field(obj, get_expr.name, slot_offset, slot_count, field_type);
        emit_ref_dec(old);
        emit_ref_inc(value);
    }

    // Destroy old field value before overwriting (prevents leaks for uniq/move-semantic fields)
    if (field_type && (field_type->kind == TypeKind::Uniq || field_type->noncopyable())) {
        emit_single_field_destroy(obj, get_expr.name, slot_offset, slot_count, field_type);
    }
    // A `string` field: release the overwritten string before storing the new one
    // (finding 9b). The field holds its own count (acquired below), so releasing on
    // reassignment reclaims it rather than leaking every overwrite.
    if (field_type && field_type->kind == TypeKind::String) {
        ValueId old_str = emit_get_field(obj, get_expr.name, slot_offset, slot_count, field_type);
        emit_str_release(old_str);
    }

    // Wrap uniq/ref → weak conversion for field assignment
    value = maybe_wrap_weak(value, assign_expr.value->resolved_type, field_type, assign_expr.value);

    // For struct-typed fields the rvalue is a struct pointer (per IR convention),
    // so we must copy slot-by-slot from the source struct. emit_set_field would
    // otherwise stuff the raw pointer bits into the field slots, silently
    // corrupting the field (e.g. losing nested when-discriminants). Mirror the
    // struct-literal initialization path for consistency.
    ValueId result;
    if (field_type && field_type->is_struct()) {
        ValueId field_addr = emit_get_field_addr(obj, get_expr.name, slot_offset, field_type);
        emit_struct_copy(field_addr, value, slot_count);
        result = value;
    } else {
        result = emit_set_field(obj, get_expr.name, slot_offset, slot_count, value, expr->resolved_type);
    }

    // Consume noncopyable temporaries assigned to fields
    if (field_type && field_type->noncopyable()) {
        consume_temp_noncopyable(value);
    }
    // A `string` field acquires its own count: retain the stored string (or adopt
    // a fresh temp) so it survives the source's release (finding 9b).
    if (field_type && field_type->kind == TypeKind::String) {
        consume_or_retain_string(value, field_type, /*adopted_by_variable=*/false);
    }

    // Move semantics: if value is a uniq/move-semantic identifier, mark it as moved
    // Only when the field type also needs move semantics (not for weak ref fields)
    if (assign_expr.value->kind == AstKind::ExprIdentifier && field_type &&
        field_type->noncopyable()) {
        Type* value_type = assign_expr.value->resolved_type;
        if (value_type && value_type->noncopyable()) {
            mark_moved_from(assign_expr.value->identifier.name);
        }
    }

    // Field-move nullify: if the RHS is a field access on a local value-struct
    // (e.g. `self.things = src.items` where `src` is a by-value noncopyable
    // struct param), the semantic analyzer marked the RHS as moved, but nothing
    // has actually cleared the source field — the enclosing struct's destructor
    // would later free it a second time. Null the source field.
    if (field_type && field_type->noncopyable()) {
        nullify_moved_field_source(assign_expr.value);
    }

    return result;
}

ValueId IRBuilder::gen_assign_index(Expr* expr, ValueId value) {
    AssignExpr& assign_expr = expr->assign;
    IndexExpr& index_expr = assign_expr.target->index;

    Type* obj_type = index_expr.object->resolved_type;
    Type* container_type = obj_type ? obj_type->base_type() : nullptr;

    // Struct indexing: dispatch to "index_mut" method
    if (container_type && container_type->is_struct()) {
        StringView method_name("index_mut", 9);
        Type* found_in = nullptr;
        const MethodInfo* method_info = lookup_method_in_hierarchy(container_type, method_name, &found_in);
        if (method_info && found_in) {
            ValueId self_ptr = gen_lvalue_addr(index_expr.object);
            ValueId index_val = gen_expr(index_expr.index);
            StringView mangled = mangle_method(found_in->struct_info.name, method_name);
            return emit_call(mangled, alloc_span({self_ptr, index_val, value}), m_types.void_type());
        }
    }

    // List/Map indexing: emit IndexSet IR op
    if (container_type && container_type->is_container()) {
        bool is_list = container_type->is_list();
        Type* elem_type = is_list ? container_type->list_info.element_type
                                  : container_type->map_info.value_type;
        bool elem_noncopyable = elem_type && elem_type->noncopyable();
        bool elem_is_ref = elem_type && elem_type->kind == TypeKind::Ref;
        ContainerKind kind = is_list ? ContainerKind::List : ContainerKind::Map;

        ValueId obj = gen_expr(index_expr.object);
        ValueId index_val = gen_expr(index_expr.index);

        // Overwriting a `ref` List element rebalances the count: release the old
        // borrow, acquire the new (the caller keeps its own copy of the new ref).
        // The index is always in bounds, so the old element always exists.
        // (lifetimes.md "Applying the model". Map<_, ref> overwrite is part of the deferred Map work.)
        if (elem_is_ref && is_list) {
            ValueId old = emit_index_get(obj, index_val, kind, elem_type);
            emit_ref_dec(old);
            emit_ref_inc(value);
        }
        // Overwriting a `string` List element: release the old, retain the new
        // (the list owns each element; finding 9b). Index is always in bounds.
        else if (elem_type && elem_type->kind == TypeKind::String && is_list) {
            ValueId old = emit_index_get(obj, index_val, kind, elem_type);
            emit_str_release(old);
            emit_str_retain(value);
        }
        // Destroy the overwritten element before storing, so a noncopyable old
        // element isn't leaked (mirrors emit_single_field_destroy for fields).
        // See docs/internals/lifetimes.md "Applying the model".
        else if (elem_noncopyable && is_list) {
            // List: the index is always in bounds, so the old element always
            // exists — destroy it unconditionally.
            ValueId old = emit_index_get(obj, index_val, kind, elem_type);
            emit_delete(old, elem_type);
        } else if (elem_noncopyable) {
            // Map: a slot has an old value only for an already-present key, so the
            // helper guards the destroy with a `contains` check (a new key destroys
            // nothing). Shared with m.insert / m.remove (lifetimes.md "Value lifecycle").
            emit_map_value_delete_if_present(obj, container_type, index_val);
        }

        emit_index_set(obj, index_val, value, kind);

        // Consume the moved-in value so it isn't double-owned (container slot +
        // caller scope). consume_temp_noncopyable handles temporaries;
        // mark_moved_from handles a named noncopyable source.
        if (elem_noncopyable) {
            consume_temp_noncopyable(value);
            if (assign_expr.value->kind == AstKind::ExprIdentifier) {
                mark_moved_from(assign_expr.value->identifier.name);
            }
        }
        return value;
    }

    return value;
}

ValueId IRBuilder::gen_grouping_expr(Expr* expr) {
    return gen_expr(expr->grouping.expr);
}

ValueId IRBuilder::gen_this_expr(Expr* expr) {
    // 'self' is the first parameter in methods
    return lookup_local("self");
}

ValueId IRBuilder::try_fold_cast(ValueId source, Type* source_type, Type* target_type) {
    if (!m_current_func) return ValueId::invalid();
    FoldedConst folded;
    if (!fold_cast_const(m_current_func->inst_for(source), source_type, target_type, folded)) {
        return ValueId::invalid();
    }
    return emit_folded_const(folded, target_type);
}

ValueId IRBuilder::gen_primitive_cast(Expr* expr) {
    CallExpr& call_expr = expr->call;

    // Get target type from callee (set by semantic analysis)
    Type* target_type = call_expr.callee->resolved_type;

    // Get source value and type
    ValueId source = gen_expr(call_expr.arguments[0].expr);
    Type* source_type = call_expr.arguments[0].expr->resolved_type;

    // If same type, no-op
    if (source_type == target_type) {
        return source;
    }

    if (ValueId folded = try_fold_cast(source, source_type, target_type); folded.is_valid()) {
        return folded;
    }

    // Emit Cast instruction
    IRInst* inst = emit_inst(IROp::Cast, target_type);
    if (inst) {
        inst->cast.source = source;
        inst->cast.source_type = source_type;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::gen_constructor_call(Expr* expr) {
    CallExpr& call_expr = expr->call;
    Type* result_type = expr->resolved_type;

    // Get struct type from callee (set by semantic analysis)
    // For Type(), callee is an identifier with struct type
    // For Type.ctor_name(), callee is a GetExpr with object having struct type
    Type* struct_type = nullptr;
    if (call_expr.callee->kind == AstKind::ExprIdentifier) {
        struct_type = call_expr.callee->resolved_type;
    } else if (call_expr.callee->kind == AstKind::ExprGet) {
        struct_type = call_expr.callee->get.object->resolved_type;
    }

    // Determine allocation mode and final struct type
    ValueId obj;
    if (call_expr.is_heap) {
        // uniq Type() - heap allocation
        // result_type is uniq<StructType>
        Span<ValueId> empty_args = {};
        obj = emit_new(struct_type->struct_info.name, empty_args, result_type);
        // Track the heap temporary for scope-exit / exception cleanup.
        // Consumed when passed/assigned/returned (marked is_moved + Nullify).
        track_noncopyable_call_temp(obj, result_type);
    } else {
        // Type() - stack allocation
        // result_type is StructType (value type)
        u32 slot_count = struct_type->struct_info.slot_count;
        obj = emit_stack_alloc(slot_count, struct_type);
    }

    // Call constructor (user-defined or synthesized)
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    StringView ctor_name = mangle_constructor(struct_type_info.name, call_expr.constructor_name);

    // Build arguments: 'self' pointer + constructor arguments.
    //
    // For noncopyable args, ownership transfers to the callee, so the shared
    // lowering consumes temps at evaluation and marks identifier args moved
    // after the call. Without that fixup `uniq T.name(arg)` where arg is a
    // uniq-typed local (or a uniq rvalue temporary) leaves the caller still
    // pointing at the slot the callee now owns — when the constructor stores
    // arg into a field and the enclosing struct is destroyed, the field
    // cleanup frees the slot AND the caller's scope-exit Delete frees it a
    // second time (slab_allocator.cpp:286 ALIVE assert).
    Span<ValueId> user_args = lower_simple_args(call_expr.arguments);
    emit_call(ctor_name, prepend_self(obj, user_args), m_types.void_type());
    mark_simple_args_moved(call_expr.arguments);

    return obj;
}

ValueId IRBuilder::gen_super_call(Expr* expr) {
    CallExpr& call_expr = expr->call;
    SuperExpr& super_expr = call_expr.callee->super_expr;

    // Get the 'self' pointer
    ValueId self_ptr = lookup_local("self");

    // Get the parent struct type from the callee's resolved_type (ref<StructType>)
    Type* ref_type = call_expr.callee->resolved_type;
    Type* target_type = nullptr;
    if (ref_type && ref_type->is_reference()) {
        target_type = ref_type->ref_info.inner_type;
    }

    if (!target_type || !target_type->is_struct()) {
        report_error("Internal error: invalid super call target");
        return ValueId::invalid();
    }

    // Constructor vs method call, from the analyzer's explicit annotation:
    // super() (empty method name) is always the parent's default constructor;
    // super.name(...) is a named constructor iff the analyzer resolved it to
    // one and recorded the name in constructor_name (a void result type does
    // NOT imply a constructor — super methods can return void).
    bool is_constructor_call = super_expr.method_name.empty()
                            || call_expr.constructor_name.size() > 0;

    StructTypeInfo& target_struct_type_info = target_type->struct_info;

    StringView call_name;
    if (is_constructor_call) {
        call_name = mangle_constructor(target_struct_type_info.name, super_expr.method_name);
    } else {
        call_name = mangle_method(target_struct_type_info.name, super_expr.method_name);
    }

    // Evaluate arguments with the shared move-aware lowering: noncopyable
    // temps are consumed at evaluation and noncopyable identifier args are
    // marked moved after the call — previously super() args skipped this
    // bookkeeping entirely, so a uniq local passed to a parent constructor's
    // owned param was freed by both the parent and the caller's scope exit.
    Span<ValueId> args = lower_simple_args(call_expr.arguments);
    Span<ValueId> call_args = prepend_self(self_ptr, args);

    // The super target may be a native function (emit_call_resolved dispatches).
    ValueId result = emit_call_resolved(call_name, call_args, expr->resolved_type);
    mark_simple_args_moved(call_expr.arguments);
    return result;
}

ValueId IRBuilder::gen_struct_literal_expr(Expr* expr) {
    StructLiteralExpr& sl = expr->struct_literal;
    Type* result_type = expr->resolved_type;

    // Determine struct type and allocation mode
    Type* struct_type;
    ValueId struct_ptr;

    if (sl.is_heap) {
        // uniq Type { ... } - heap allocation
        struct_type = result_type->ref_info.inner_type;
        Span<ValueId> empty_args = {};
        // Use mangled name for generic struct instances (e.g., "Box$i32")
        StringView type_name = sl.mangled_name.size() > 0 ? sl.mangled_name : sl.type_name;
        struct_ptr = emit_new(type_name, empty_args, result_type);
        // Track the heap temporary for scope-exit / exception cleanup.
        track_noncopyable_call_temp(struct_ptr, result_type);
    } else {
        // Type { ... } - stack allocation
        struct_type = result_type;
        u32 slot_count = struct_type->struct_info.slot_count;
        struct_ptr = emit_stack_alloc(slot_count, struct_type);
    }

    // Build map of provided field initializers
    tsl::robin_map<StringView, Expr*> provided_fields;
    for (auto& field : sl.fields) {
        provided_fields[field.name] = field.value;
    }

    // Helper to find default value for a field by searching the inheritance chain
    auto find_field_default = [](Type* type, StringView field_name) -> Expr* {
        Type* current = type;
        while (current && current->is_struct()) {
            if (!current->struct_info.decl) {
                current = current->struct_info.parent;
                continue;
            }
            StructDecl& struct_decl = current->struct_info.decl->struct_decl;
            for (auto& field : struct_decl.fields) {
                if (field.name == field_name) {
                    return field.default_value;
                }
            }
            current = current->struct_info.parent;
        }
        return nullptr;
    };

    // Initialize regular fields (including discriminants which are in struct_info.fields)
    for (auto& field_info : struct_type->struct_info.fields) {
        Expr* value_expr = nullptr;

        auto it = provided_fields.find(field_info.name);
        if (it != provided_fields.end()) {
            value_expr = it->second;
        } else {
            // Use default value from struct declaration (searching inheritance chain)
            value_expr = find_field_default(struct_type, field_info.name);
        }

        ValueId value = gen_expr(value_expr);

        // Wrap uniq/ref → weak conversion for struct literal field
        if (value_expr) {
            value = maybe_wrap_weak(value, value_expr->resolved_type, field_info.type, value_expr);
        }

        // For struct-typed fields, use StructCopy since the value is a pointer
        if (field_info.type && field_info.type->is_struct()) {
            // Get address of the field
            ValueId field_addr = emit_get_field_addr(struct_ptr, field_info.name, field_info.slot_offset, field_info.type);
            // Copy struct data from value (source pointer) to field_addr (dest pointer)
            emit_struct_copy(field_addr, value, field_info.slot_count);
        } else {
            emit_set_field(struct_ptr, field_info.name, field_info.slot_offset, field_info.slot_count, value, field_info.type);
        }

        // A `ref` field is a counted borrow: storing a borrow into it acquires a
        // count on the pointee (released on struct drop; lifetimes.md "Value lifecycle"
        // step 3). The struct is move-only, so the source stays live — no move.
        if (field_info.type && field_info.type->kind == TypeKind::Ref && value_expr) {
            emit_ref_inc(value);
        }

        // A `string` field: retain the stored string (or adopt a fresh temp) so the
        // field holds its own count and doesn't dangle when the source is released
        // (finding 9b). Structs stay copyable/trivial, so the field is not released
        // on struct drop — a string held in a struct field is a bounded leak, the
        // same as before this change; the retain only prevents a use-after-free.
        if (field_info.type && field_info.type->kind == TypeKind::String && value_expr) {
            consume_or_retain_string(value, field_info.type, /*adopted_by_variable=*/false);
        }

        // Consume noncopyable temporaries moved into struct fields
        if (field_info.type && field_info.type->noncopyable() && value_expr) {
            consume_temp_noncopyable(value);
        }

        // Nullify source variable when moving a noncopyable value into a regular field
        if (field_info.type && field_info.type->noncopyable() && value_expr) {
            if (value_expr->kind == AstKind::ExprIdentifier) {
                mark_moved_from(value_expr->identifier.name);
            }
            // `Foo { x = o.field }`: null the moved-out source field in its root.
            nullify_moved_field_source(value_expr);
        }
    }

    // Initialize variant fields from when clauses
    for (const auto& clause : struct_type->struct_info.when_clauses) {

        // For each variant in the clause
        for (const auto& variant : clause.variants) {

            // Initialize variant fields if they're provided
            for (const auto& variant_field_info : variant.fields) {

                auto it = provided_fields.find(variant_field_info.name);
                if (it != provided_fields.end()) {
                    Expr* value_expr = it->second;
                    ValueId value = gen_expr(value_expr);

                    // Compute the actual offset: union_slot_offset + variant field's offset
                    u32 actual_slot_offset = clause.union_slot_offset + variant_field_info.slot_offset;

                    // For struct-typed variant fields, use StructCopy
                    if (variant_field_info.type && variant_field_info.type->is_struct()) {
                        ValueId field_addr = emit_get_field_addr(struct_ptr, variant_field_info.name, actual_slot_offset, variant_field_info.type);
                        emit_struct_copy(field_addr, value, variant_field_info.slot_count);
                    } else {
                        emit_set_field(struct_ptr, variant_field_info.name, actual_slot_offset, variant_field_info.slot_count, value, variant_field_info.type);
                    }

                    // Consume noncopyable temporaries moved into variant fields
                    if (variant_field_info.type && variant_field_info.type->noncopyable()) {
                        consume_temp_noncopyable(value);
                    }

                    // Nullify source variable when moving a noncopyable value into a variant field
                    if (variant_field_info.type && variant_field_info.type->noncopyable()) {
                        if (value_expr->kind == AstKind::ExprIdentifier) {
                            mark_moved_from(value_expr->identifier.name);
                        }
                        nullify_moved_field_source(value_expr);
                    }
                }
            }
        }
    }

    return struct_ptr;
}

ValueId IRBuilder::gen_static_get_expr(Expr* expr) {
    StaticGetExpr& sge = expr->static_get;

    // Currently only enum variants use static get (Type::Variant). Resolve
    // through the enum type's own variant table (expr->resolved_type is the
    // enum) — the flat symbol namespace would return a same-named variant of
    // whichever enum was defined last, emitting the wrong constant.
    Type* enum_type = expr->resolved_type;
    if (enum_type && enum_type->is_enum()) {
        if (const EnumVariantInfo* variant =
                enum_type->enum_info.find_variant(sge.member_name)) {
            return emit_const_int(variant->value, enum_type);
        }
    }

    // Should not reach here if semantic analysis passed
    report_error("Internal error: unexpected state in static get expression");
    return ValueId::invalid();
}

ValueId IRBuilder::gen_string_interp_expr(Expr* expr) {
    auto& string_interp = expr->string_interp;
    Type* string_type = m_types.string_type();

    // Build a flat list of string-valued ValueIds
    Vector<ValueId> string_parts;

    for (u32 i = 0; i < string_interp.parts.size(); i++) {
        // Add text part if non-empty
        if (string_interp.parts[i].size() > 0) {
            string_parts.push_back(emit_const_string(string_interp.parts[i]));
        }

        // Add expression part (if there is one — there are N expressions for N+1 parts)
        if (i < string_interp.expressions.size()) {
            Expr* sub = string_interp.expressions[i];
            Type* etype = sub->resolved_type;
            ValueId val = gen_expr(sub);

            if (etype->kind == TypeKind::String) {
                // String expression — use directly, no conversion needed
                string_parts.push_back(val);
            } else {
                // Need to call to_string native for this type
                const char* native_name = nullptr;
                switch (etype->kind) {
                    case TypeKind::Bool:   native_name = "bool$$to_string"; break;
                    case TypeKind::I32:    native_name = "i32$$to_string"; break;
                    case TypeKind::I64:    native_name = "i64$$to_string"; break;
                    case TypeKind::U32:    native_name = "u32$$to_string"; break;
                    case TypeKind::U64:    native_name = "u64$$to_string"; break;
                    case TypeKind::F32:    native_name = "f32$$to_string"; break;
                    case TypeKind::F64:    native_name = "f64$$to_string"; break;
                    default: break;
                }

                if (etype->is_enum()) {
                    // Enums use i32$$to_string on their underlying value
                    native_name = "i32$$to_string";
                }

                if (native_name) {
                    StringView name(native_name, static_cast<u32>(strlen(native_name)));
                    ValueId ts = emit_native(name, {val}, string_type);
                    // The to_string result is a fresh owned string temp — track
                    // it so it's released at scope exit (finding 9b).
                    track_string_temp(ts, string_type);
                    string_parts.push_back(ts);
                } else if (etype->is_struct()) {
                    // Struct with to_string method: call the mangled method.
                    // gen_expr already returns a struct pointer for struct rvalues — the
                    // lowering pass unpacks struct returns into stack-allocated pointers
                    // (see the note in gen_var_decl). Reusing `val` here avoids a second
                    // pass through gen_lvalue_addr, which doesn't accept call/index/method
                    // rvalues and would error with "expression is not a valid lvalue".
                    StringView mangled = mangle_method(etype->struct_info.name,
                                                       "to_string"_sv);
                    ValueId ts = emit_call_resolved(mangled, alloc_span({val}), string_type);
                    // A user `to_string()` hands off an owned string — track it.
                    track_string_temp(ts, string_type);
                    string_parts.push_back(ts);
                }
            }
        }
    }

    // Edge case: empty f-string or no parts
    if (string_parts.empty()) {
        return emit_const_string(""_sv);
    }

    // Left-fold concatenation with str_concat
    ValueId result = string_parts[0];
    StringView concat_name("str_concat", 10);
    i32 concat_idx = m_registry.get_index(concat_name);

    for (u32 i = 1; i < string_parts.size(); i++) {
        result = emit_call_native(concat_name, alloc_span({result, string_parts[i]}),
                                  string_type, static_cast<u32>(concat_idx));
        // Each concat produces a fresh owned string temp; track it for release at
        // scope exit (finding 9b). The final `result` is returned and re-tracked by
        // gen_expr (track_string_temp skips already-tracked values).
        track_string_temp(result, string_type);
    }

    return result;
}

ValueId IRBuilder::gen_lvalue_addr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprIdentifier: {
            StringView name = expr->identifier.name;
            // If this is already a pointer parameter, return its value directly
            if (m_param_is_ptr.count(name)) {
                return lookup_local(name);
            }

            ValueId current_val = lookup_local(name);
            Type* type = expr->resolved_type;

            // For struct types, the variable is already a pointer to stack-allocated data.
            // Just return the existing pointer - no copy needed.
            if (type && type->is_struct()) {
                return current_val;
            }

            // For primitive types and list pointers, we need to:
            // 1. Allocate a stack slot
            // 2. Store the current value to the slot
            // 3. Return the address

            // Calculate slot count. Structs returned above; get_type_slot_count covers every
            // remaining width (weak=4, uniq/ref/list/map/string/fn=2). 0 => opaque, default 1.
            u32 slot_count = slot_count_or_1(type);

            // Allocate stack space
            ValueId addr = emit_stack_alloc(slot_count, type);

            // Store current value to the stack slot (using SetField with offset 0)
            emit_set_field(addr, name, 0, slot_count, current_val, type);

            return addr;
        }
        case AstKind::ExprGet: {
            // Field access - use GET_FIELD_ADDR
            GetExpr& get_expr = expr->get;
            ValueId obj = gen_expr(get_expr.object);

            // Get the struct type from the object
            Type* obj_type = get_expr.object->resolved_type;
            Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

            u32 slot_offset = 0;
            if (struct_type && struct_type->is_struct()) {
                const FieldInfo* field_info = struct_type->struct_info.find_field(get_expr.name);
                if (field_info) {
                    slot_offset = field_info->slot_offset;
                }
            }

            return emit_get_field_addr(obj, get_expr.name, slot_offset, expr->resolved_type);
        }
        case AstKind::ExprIndex: {
            // Address of a List/Map element (out/inout argument). The runtime
            // returns a pointer into the backing buffer, valid for the borrow
            // because the call site pins the container (lifetimes.md "Container element lvalues"). The
            // element type is the subscript's resolved (borrowed) type.
            IndexExpr& index_expr = expr->index;
            Type* obj_type = index_expr.object->resolved_type;
            Type* base_type = obj_type ? obj_type->base_type() : nullptr;
            if (base_type && base_type->is_container()) {
                ValueId obj = gen_expr(index_expr.object);
                ValueId idx = gen_expr(index_expr.index);
                ContainerKind kind = base_type->is_list() ? ContainerKind::List
                                                          : ContainerKind::Map;
                return emit_index_addr(obj, idx, kind, expr->resolved_type);
            }
            report_error("Internal error: cannot take the address of this index expression");
            return ValueId::invalid();
        }
        case AstKind::ExprGrouping:
            return gen_lvalue_addr(expr->grouping.expr);
        default:
            // Should not happen - semantic analysis validated lvalues
            report_error("Internal error: expression is not a valid lvalue");
            return ValueId::invalid();
    }
}

// Declaration generation

IROp IRBuilder::get_binary_op(BinaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;
    // Unsigned discrimination (U32 and U64): add/sub/mul/shl/bitwise are
    // bit-identical for signed/unsigned, so only div/mod/shr diverge. u32 reuses
    // the same unsigned IR ops as u64 — correct because u32 values are kept
    // canonically zero-extended (lowering's TRUNC_U 32 hook), so they're
    // non-negative in i64 and the 64-bit unsigned ops give the right answer.
    bool is_unsigned = type && (type->kind == TypeKind::U32 || type->kind == TypeKind::U64);

    switch (op) {
        case BinaryOp::Add:
            return is_f32 ? IROp::AddF : (is_f64 ? IROp::AddD : IROp::AddI);
        case BinaryOp::Sub:
            return is_f32 ? IROp::SubF : (is_f64 ? IROp::SubD : IROp::SubI);
        case BinaryOp::Mul:
            return is_f32 ? IROp::MulF : (is_f64 ? IROp::MulD : IROp::MulI);
        case BinaryOp::Div:
            return is_f32 ? IROp::DivF : (is_f64 ? IROp::DivD : (is_unsigned ? IROp::DivU : IROp::DivI));
        case BinaryOp::Mod:
            return is_unsigned ? IROp::ModU : IROp::ModI;
        case BinaryOp::BitAnd:
            return IROp::BitAnd;
        case BinaryOp::BitOr:
            return IROp::BitOr;
        case BinaryOp::BitXor:
            return IROp::BitXor;
        case BinaryOp::Shl:
            return IROp::Shl;
        case BinaryOp::Shr:
            return is_unsigned ? IROp::UShr : IROp::Shr;
        default:
            return IROp::Copy;
    }
}

IROp IRBuilder::get_comparison_op(BinaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;
    // eq/ne are bit compares (same signed/unsigned); only ordered compares diverge.
    // u32 and u64 both use the unsigned compares (canonical zero-extended operands).
    bool is_unsigned = type && (type->kind == TypeKind::U32 || type->kind == TypeKind::U64);

    switch (op) {
        case BinaryOp::Equal:
            return is_f32 ? IROp::EqF : (is_f64 ? IROp::EqD : IROp::EqI);
        case BinaryOp::NotEqual:
            return is_f32 ? IROp::NeF : (is_f64 ? IROp::NeD : IROp::NeI);
        case BinaryOp::Less:
            return is_f32 ? IROp::LtF : (is_f64 ? IROp::LtD : (is_unsigned ? IROp::LtU : IROp::LtI));
        case BinaryOp::LessEq:
            return is_f32 ? IROp::LeF : (is_f64 ? IROp::LeD : (is_unsigned ? IROp::LeU : IROp::LeI));
        case BinaryOp::Greater:
            return is_f32 ? IROp::GtF : (is_f64 ? IROp::GtD : (is_unsigned ? IROp::GtU : IROp::GtI));
        case BinaryOp::GreaterEq:
            return is_f32 ? IROp::GeF : (is_f64 ? IROp::GeD : (is_unsigned ? IROp::GeU : IROp::GeI));
        default:
            return IROp::EqI;
    }
}

IROp IRBuilder::get_unary_op(UnaryOp op, Type* type) {
    bool is_f32 = type && type->kind == TypeKind::F32;
    bool is_f64 = type && type->kind == TypeKind::F64;

    switch (op) {
        case UnaryOp::Negate:
            return is_f32 ? IROp::NegF : (is_f64 ? IROp::NegD : IROp::NegI);
        case UnaryOp::Not:
            return IROp::Not;
        case UnaryOp::BitNot:
            return IROp::BitNot;
        case UnaryOp::Ref:
            return IROp::Copy;  // Handled specially in gen_unary_expr
    }
    return IROp::Copy;
}

// Name mangling helpers

}

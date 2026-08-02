#include "roxy/compiler/coroutine_lowering.hpp"
#include "roxy/compiler/mangling.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/core/format.hpp"

#include <cassert>
#include <cstring>

namespace rx {

// ===== Helpers =====

template<typename T>
static Span<T> alloc_span(BumpAllocator& allocator, const Vector<T>& vec) {
    if (vec.empty()) return {};
    T* data = reinterpret_cast<T*>(allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T)));
    for (u32 i = 0; i < vec.size(); i++) {
        data[i] = vec[i];
    }
    return Span<T>(data, static_cast<u32>(vec.size()));
}

template<typename T>
static Span<T> alloc_span(BumpAllocator& allocator, u32 count) {
    if (count == 0) return {};
    T* data = reinterpret_cast<T*>(allocator.alloc_bytes(sizeof(T) * count, alignof(T)));
    return Span<T>(data, count);
}

static StringView alloc_string(BumpAllocator& allocator, const char* str) {
    u32 len = 0;
    while (str[len]) len++;
    char* buf = reinterpret_cast<char*>(allocator.alloc_bytes(len, 1));
    memcpy(buf, str, len);
    return StringView(buf, len);
}

static StringView alloc_string_fmt(BumpAllocator& allocator, const char* fmt, StringView arg) {
    return format_to_arena(allocator, runtime(fmt), arg);
}

static constexpr i32 CORO_STATE_DONE = 0x7FFFFFFF;

struct YieldPoint {
    u32 block_index;
    u32 inst_index;
    ValueId yielded_value;
    BlockId resume_block_id;
};

struct PromotedVar {
    StringView name;
    Type* type;
    u32 field_slot_offset;
    u32 field_slot_count;
};

// get_type_slot_count is declared in types.hpp and defined in types.cpp.

static IRInst* make_inst(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                          IROp op, Type* type) {
    IRInst* inst = allocator.emplace<IRInst>();
    inst->op = op;
    inst->type = type;
    inst->result = func->new_value_for(inst);
    block->instructions.push_back(inst);
    return inst;
}

static ValueId emit_const_int(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               i64 value, Type* type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::ConstInt, type);
    inst->const_data.int_val = value;
    return inst->result;
}

static ValueId emit_new(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                         StringView type_name, Type* result_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::New, result_type);
    inst->new_data.type_name = type_name;
    inst->new_data.args = Span<ValueId>();
    return inst->result;
}

static ValueId emit_get_field(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               ValueId object, StringView field_name,
                               u32 slot_offset, u32 slot_count, Type* result_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::GetField, result_type);
    inst->field.object = object;
    inst->field.field_name = field_name;
    inst->field.slot_offset = slot_offset;
    inst->field.slot_count = slot_count;
    return inst->result;
}

static ValueId emit_get_field_addr(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                                    ValueId object, StringView field_name,
                                    u32 slot_offset, u32 slot_count, Type* field_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::GetFieldAddr, field_type);
    inst->field.object = object;
    inst->field.field_name = field_name;
    inst->field.slot_offset = slot_offset;
    inst->field.slot_count = slot_count;
    return inst->result;
}

static ValueId emit_set_field(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               ValueId object, StringView field_name,
                               u32 slot_offset, u32 slot_count, ValueId value, Type* type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::SetField, type);
    inst->field.object = object;
    inst->field.field_name = field_name;
    inst->field.slot_offset = slot_offset;
    inst->field.slot_count = slot_count;
    inst->store_value = value;
    return inst->result;
}

// Copy a value struct into a field of `object`, rather than storing a register
// into it. `source_ptr` is the struct's address (how the IR represents a value
// struct); a plain SetField would store that pointer into a field sized for the
// struct's slots, so the reader would decode an address as struct contents.
static void emit_set_struct_field(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                                  ValueId object, StringView field_name,
                                  u32 slot_offset, u32 slot_count,
                                  ValueId source_ptr, Type* field_type) {
    IRInst* addr = make_inst(allocator, func, block, IROp::GetFieldAddr, field_type);
    addr->field.object = object;
    addr->field.field_name = field_name;
    addr->field.slot_offset = slot_offset;
    addr->field.slot_count = slot_count;

    IRInst* copy = make_inst(allocator, func, block, IROp::StructCopy, field_type);
    copy->struct_copy.dest_ptr = addr->result;
    copy->struct_copy.source_ptr = source_ptr;
    copy->struct_copy.slot_count = slot_count;
}

static ValueId emit_eq_i(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                           ValueId left, ValueId right, Type* bool_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::EqI, bool_type);
    inst->binary.left = left;
    inst->binary.right = right;
    return inst->result;
}

static void finish_goto(BumpAllocator& allocator, IRBlock* block, BlockId target,
                         Span<BlockArgPair> args = {}) {
    block->terminator.kind = TerminatorKind::Goto;
    block->terminator.goto_target.block = target;
    block->terminator.goto_target.args = args;
}

static void finish_branch(IRBlock* block, ValueId cond,
                           BlockId then_block, BlockId else_block) {
    block->terminator.kind = TerminatorKind::Branch;
    block->terminator.branch.condition = cond;
    block->terminator.branch.then_target.block = then_block;
    block->terminator.branch.then_target.args = {};
    block->terminator.branch.else_target.block = else_block;
    block->terminator.branch.else_target.args = {};
}

static void finish_return(IRBlock* block, ValueId value) {
    block->terminator.kind = TerminatorKind::Return;
    block->terminator.return_value = value;
}

static void finish_unreachable(IRBlock* block) {
    block->terminator.kind = TerminatorKind::Unreachable;
}

static IRBlock* create_block(BumpAllocator& allocator, IRFunction* func, StringView name) {
    IRBlock* block = allocator.emplace<IRBlock>();
    block->id = BlockId{static_cast<u32>(func->blocks.size())};
    block->name = name;
    func->blocks.push_back(block);
    return block;
}

static ValueId emit_const_null(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                                Type* nil_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::ConstNull, nil_type);
    return inst->result;
}

static ValueId emit_call(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                           StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::Call, result_type);
    inst->call.func_name = func_name;
    inst->call.args = args;
    inst->call.native_index = 0;
    return inst->result;
}

static ValueId emit_func_index(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                                StringView func_name, Type* u32_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::FuncIndex, u32_type);
    inst->func_index.func_name = func_name;
    return inst->result;
}

// Emit a void-typed unary op on a single value operand: Delete, RefInc/RefDec
// (constraint-reference counting — a `ref` promoted into the state struct is a
// counted borrow held for the state's lifetime: RefInc at creation, RefDec in
// $$delete), or AssertHeap.
static void emit_unary_void_op(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               IROp op, ValueId value, Type* void_type) {
    IRInst* inst = make_inst(allocator, func, block, op, void_type);
    inst->unary = value;
}

// Generate the __coro_<func_name>$$delete destructor function.
// This iterates promoted struct fields in reverse order (LIFO) and cleans up
// any noncopyable pointer-type fields (uniq, List, Map, Coro).
static IRFunction* generate_coro_destructor(BumpAllocator& allocator, Type* struct_type,
                                             StringView func_name, TypeCache& types,
                                             IRModule* module,
                                             const Vector<BlockParam>& original_params,
                                             const tsl::robin_map<StringView, bool>& catch_names) {
    IRFunction* dtor_func = allocator.emplace<IRFunction>();
    StringView dtor_name = alloc_string_fmt(allocator, "__coro_{}$$delete", func_name);
    dtor_func->name = dtor_name;
    dtor_func->return_type = types.void_type();

    // A `ref`-typed state field is a counted borrow (a ref *parameter* acquired
    // at init, or a ref *local* acquired mid-body) and gets a null-guarded
    // RefDec here: the field is non-null exactly when the borrow is still held
    // at destroy (a ref local nulls its field when released on the resume path —
    // see the Nullify→SetField pass in coroutine_lower). The one exception is a
    // catch param `e`: a `ref` field set by exception *dispatch*, never counted,
    // so it must be left alone. `catch_names` holds those names.
    auto is_catch_param_field = [&](StringView name) -> bool {
        return catch_names.count(name) != 0;
    };

    // Single parameter: self: ref<__coro_*>
    Type* ref_struct_type = types.ref_type(struct_type);
    BlockParam self_param;
    self_param.value = dtor_func->new_value();
    self_param.type = ref_struct_type;
    self_param.name = alloc_string(allocator, "self");
    dtor_func->params.push_back(self_param);
    dtor_func->param_is_ptr.push_back(false);

    ValueId self_val = self_param.value;
    IRBlock* entry = create_block(allocator, dtor_func, alloc_string(allocator, "entry"));

    // Process fields in reverse order (LIFO, like C++ member destruction).
    // Skip the three reserved header fields __resume_idx, __state, __yield_val
    // (indices 0, 1, 2) — only params/promoted locals own resources.
    const auto& fields = struct_type->struct_info.fields;
    for (i32 i = static_cast<i32>(fields.size()) - 1; i >= 3; i--) {
        const FieldInfo& field = fields[i];
        if (!field.type) continue;

        // Which fields need cleanup is the shared, non-recursive decision
        // (`member_needs_drop`: noncopyable, or a bare `ref`) — the same one the
        // synthetic-destructor pass and IRBuilder::emit_field_cleanup use. It
        // was hand-enumerated here as "uniq | noncopyable container | Coro",
        // which silently skipped every other owning shape: once value structs
        // began living *inline* in the state struct, a promoted struct owning a
        // `uniq` was never destroyed at all.
        if (!member_needs_drop(field.type)) continue;

        // A `ref` field is a counted borrow (ref param acquired at init, or ref
        // local acquired mid-body); release it here (RefDec the borrowed pointer,
        // never free the pointee), guarded by the null check below so an
        // already-released local is skipped. Only a catch param `e` is excluded —
        // it's set by exception dispatch, not counted. ("Applying the model".)
        bool is_ref = field.type->kind == TypeKind::Ref;
        if (is_ref && is_catch_param_field(field.name)) continue;

        // An inline value struct *is* its storage — there is no pointer to
        // null-check, and no object to free. Address it and run a typed Delete,
        // which lowers to its destructor in both backends (the same shape as
        // IRBuilder::emit_single_field_destroy's value-struct arm).
        if (field.type->is_struct()) {
            ValueId field_addr = emit_get_field_addr(allocator, dtor_func, entry, self_val,
                                                     field.name, field.slot_offset,
                                                     field.slot_count, field.type);
            emit_unary_void_op(allocator, dtor_func, entry, IROp::Delete,
                               field_addr, field.type);
            continue;
        }

        // GetField → null check → call inner destructor → Delete → skip
        ValueId field_val = emit_get_field(allocator, dtor_func, entry, self_val,
                                            field.name, field.slot_offset, field.slot_count,
                                            field.type);
        ValueId null_val = emit_const_null(allocator, dtor_func, entry, types.nil_type());
        ValueId is_null = emit_eq_i(allocator, dtor_func, entry,
                                     field_val, null_val, types.bool_type());

        char cleanup_name[64];
        format_to(cleanup_name, sizeof(cleanup_name), "field_cleanup_{}", i);
        char skip_name[64];
        format_to(skip_name, sizeof(skip_name), "field_skip_{}", i);

        IRBlock* cleanup_block = create_block(allocator, dtor_func,
                                               alloc_string(allocator, cleanup_name));
        IRBlock* skip_block = create_block(allocator, dtor_func,
                                            alloc_string(allocator, skip_name));

        finish_branch(entry, is_null, skip_block->id, cleanup_block->id);

        // In cleanup block: release the resource. A `ref` field only releases its
        // borrow count (the owner frees the pointee); owning fields run their
        // destructor + free.
        if (is_ref) {
            emit_unary_void_op(allocator, dtor_func, cleanup_block, IROp::RefDec,
                               field_val, types.void_type());
        } else if (field.type->kind == TypeKind::Uniq) {
            Type* inner_type = field.type->ref_info.inner_type;
            if (inner_type && inner_type->is_struct()) {
                if (struct_has_default_dtor(inner_type)) {
                    StringView inner_dtor_sv =
                        mangle_destructor(allocator, inner_type->struct_info.name);
                    Span<ValueId> call_args = alloc_span<ValueId>(allocator, 1);
                    call_args[0] = field_val;
                    emit_call(allocator, dtor_func, cleanup_block,
                              inner_dtor_sv, call_args, types.void_type());
                }
            }
            emit_unary_void_op(allocator, dtor_func, cleanup_block, IROp::Delete,
                               field_val, types.void_type());
        } else if (field.type->is_coroutine()) {
            // Recursively call the coroutine's destructor
            StringView coro_func_name = field.type->coro_info.func_name;
            StringView coro_dtor_name = alloc_string_fmt(allocator,
                "__coro_{}$$delete", coro_func_name);
            Span<ValueId> call_args = alloc_span<ValueId>(allocator, 1);
            call_args[0] = field_val;
            emit_call(allocator, dtor_func, cleanup_block,
                      coro_dtor_name, call_args, types.void_type());
            emit_unary_void_op(allocator, dtor_func, cleanup_block, IROp::Delete,
                               field_val, types.void_type());
        }
        else if (field.type->is_container()) {
            auto wrapper_it = module->cleanup_wrappers.find(field.type);
            if (wrapper_it != module->cleanup_wrappers.end()) {
                // Call the cleanup wrapper (handles element cleanup + buffer free + Delete)
                Span<ValueId> call_args = alloc_span<ValueId>(allocator, 1);
                call_args[0] = field_val;
                emit_call(allocator, dtor_func, cleanup_block,
                          wrapper_it->second, call_args, types.void_type());
            } else {
                // Fallback: bare Delete (no element cleanup)
                emit_unary_void_op(allocator, dtor_func, cleanup_block, IROp::Delete,
                                   field_val, types.void_type());
            }
        }
        else {
            // Any other owning pointer shape — today a closure (`fun(..) -> R`
            // owns its heap env). A typed Delete lets each backend's drop
            // derivation dispatch it, rather than needing an arm here.
            emit_unary_void_op(allocator, dtor_func, cleanup_block, IROp::Delete,
                               field_val, field.type);
        }

        finish_goto(allocator, cleanup_block, skip_block->id);
        entry = skip_block;  // Continue from skip block for next field
    }

    // Return void from the last block
    ValueId void_val = emit_const_int(allocator, dtor_func, entry, 0, types.void_type());
    finish_return(entry, void_val);

    dtor_func->reorder_blocks_rpo();
    return dtor_func;
}

// ===== In-place value remapping =====

static ValueId remap_value(const tsl::robin_map<u32, ValueId>& value_map, ValueId vid) {
    auto it = value_map.find(vid.id);
    return (it != value_map.end()) ? it->second : vid;
}

static void remap_jump_args(const tsl::robin_map<u32, ValueId>& value_map, JumpTarget& target) {
    for (u32 i = 0; i < target.args.size(); i++) {
        target.args[i].value = remap_value(value_map, target.args[i].value);
    }
}

static void remap_inst_values(const tsl::robin_map<u32, ValueId>& value_map, IRInst* inst) {
    switch (inst->op) {
        case IROp::ConstNull: case IROp::ConstBool: case IROp::ConstInt:
        case IROp::ConstF: case IROp::ConstD: case IROp::ConstString:
        case IROp::StackAlloc: case IROp::BlockArg:
            break;
        case IROp::GetField: case IROp::GetFieldAddr:
            inst->field.object = remap_value(value_map, inst->field.object);
            break;
        case IROp::SetField:
            inst->field.object = remap_value(value_map, inst->field.object);
            inst->store_value = remap_value(value_map, inst->store_value);
            break;
        case IROp::New:
            for (u32 i = 0; i < inst->new_data.args.size(); i++)
                inst->new_data.args[i] = remap_value(value_map, inst->new_data.args[i]);
            break;
        case IROp::Call: case IROp::CallNative:
            for (u32 i = 0; i < inst->call.args.size(); i++)
                inst->call.args[i] = remap_value(value_map, inst->call.args[i]);
            break;
        case IROp::CallExternal:
            for (u32 i = 0; i < inst->call_external.args.size(); i++)
                inst->call_external.args[i] = remap_value(value_map, inst->call_external.args[i]);
            break;
        case IROp::CallIndirect:
            inst->call_indirect.callee = remap_value(value_map, inst->call_indirect.callee);
            for (u32 i = 0; i < inst->call_indirect.args.size(); i++)
                inst->call_indirect.args[i] = remap_value(value_map, inst->call_indirect.args[i]);
            break;
        case IROp::Closure:
            for (u32 i = 0; i < inst->closure.captures.size(); i++)
                inst->closure.captures[i] = remap_value(value_map, inst->closure.captures[i]);
            break;
        case IROp::StructCopy:
            inst->struct_copy.dest_ptr = remap_value(value_map, inst->struct_copy.dest_ptr);
            inst->struct_copy.source_ptr = remap_value(value_map, inst->struct_copy.source_ptr);
            break;
        case IROp::LoadPtr:
            inst->load_ptr.ptr = remap_value(value_map, inst->load_ptr.ptr);
            break;
        case IROp::StorePtr:
            inst->store_ptr.ptr = remap_value(value_map, inst->store_ptr.ptr);
            inst->store_ptr.value = remap_value(value_map, inst->store_ptr.value);
            break;
        case IROp::Cast:
            inst->cast.source = remap_value(value_map, inst->cast.source);
            break;
        case IROp::AssertHeap:
            inst->unary = remap_value(value_map, inst->unary);
            break;
        default:
            // Unary/binary ops
            inst->unary = remap_value(value_map, inst->unary);
            if (inst->op >= IROp::AddI && inst->op <= IROp::UShr) {
                inst->binary.left = remap_value(value_map, inst->binary.left);
                inst->binary.right = remap_value(value_map, inst->binary.right);
            }
            break;
    }
}

static void remap_terminator_values(const tsl::robin_map<u32, ValueId>& value_map,
                                     Terminator& term) {
    switch (term.kind) {
        case TerminatorKind::Goto:
            remap_jump_args(value_map, term.goto_target);
            break;
        case TerminatorKind::Branch:
            term.branch.condition = remap_value(value_map, term.branch.condition);
            remap_jump_args(value_map, term.branch.then_target);
            remap_jump_args(value_map, term.branch.else_target);
            break;
        case TerminatorKind::Return:
            term.return_value = remap_value(value_map, term.return_value);
            break;
        default:
            break;
    }
}

static void remap_all_block_ids(IRFunction* func, const tsl::robin_map<u32, u32>& block_map) {
    auto remap_id = [&](BlockId& bid) {
        auto it = block_map.find(bid.id);
        if (it != block_map.end()) bid.id = it->second;
    };
    auto remap_target = [&](JumpTarget& target) {
        remap_id(target.block);
    };
    for (auto* block : func->blocks) {
        switch (block->terminator.kind) {
            case TerminatorKind::Goto:
                remap_target(block->terminator.goto_target);
                break;
            case TerminatorKind::Branch:
                remap_target(block->terminator.branch.then_target);
                remap_target(block->terminator.branch.else_target);
                break;
            default:
                break;
        }
    }
    // Remap exception handler BlockIds
    for (auto& handler : func->exception_handlers) {
        remap_id(handler.try_entry);
        remap_id(handler.try_exit);
        remap_id(handler.handler_block);
        for (BlockId& bid : handler.try_body_blocks) remap_id(bid);
    }
}

// A promoted variable whose storage lives *inline* in the coroutine state
// struct rather than round-tripping through a register.
//
// True for plain value structs. Their SSA value is an address, and the state
// field is sized to hold the whole struct (`get_type_slot_count`), so the state
// *is* the variable's storage: the body reads the field's address and mutates
// it in place. Reference-shaped types (`uniq`/`ref`/`weak`, containers, `Coro`)
// are single pointer values and keep the by-value GetField/SetField path.
static bool is_inline_struct_var(Type* type) {
    return type && type->is_struct();
}

// ===== Phase 1: Promote variables to struct fields =====

struct BlockParamAnalysis {
    Vector<BlockParam> original_params;
    Vector<u32> promoted_indices;
    Vector<u32> non_promoted_indices;
};

static void phase1_promote(IRFunction* func, BumpAllocator& allocator,
                            const Vector<PromotedVar>& promoted_vars,
                            const tsl::robin_map<StringView, u32>& promoted_var_index,
                            ValueId self_val, Type* ref_struct_type) {
    // Build set of exception param (block_id, param_index) pairs.
    // These are catch block parameters set by VM exception dispatch and must NOT
    // be treated as promoted vars (even if the name collides).
    struct ExceptionParamInfo {
        u32 block_id;
        u32 param_index;
    };
    Vector<ExceptionParamInfo> exception_params;
    tsl::robin_map<u32, bool> exception_param_block_ids;
    for (auto& handler : func->exception_handlers) {
        IRBlock* handler_block = func->blocks[handler.handler_block.id];
        if (!handler_block->params.empty()) {
            exception_params.push_back({handler.handler_block.id, 0});
            exception_param_block_ids[handler.handler_block.id] = true;
        }
    }

    auto is_exception_param = [&](u32 block_id, u32 param_index) -> bool {
        for (auto& ep : exception_params) {
            if (ep.block_id == block_id && ep.param_index == param_index) return true;
        }
        return false;
    };

    // 5a. Save and replace function params with self
    Vector<BlockParam> old_params = func->params;
    func->params.clear();
    BlockParam self_param;
    self_param.value = self_val;
    self_param.type = ref_struct_type;
    self_param.name = alloc_string(allocator, "self");
    func->params.push_back(self_param);
    func->param_is_ptr.clear();
    func->param_is_ptr.push_back(false);

    // 5b. Collect ALL original ValueIds for each promoted var
    // (from function params and block params across all blocks)
    // Skip exception params — they are set by the VM, not by SSA data flow.
    tsl::robin_map<StringView, Vector<u32>> all_promoted_value_ids;
    for (auto& param : old_params) {
        if (promoted_var_index.count(param.name)) {
            all_promoted_value_ids[param.name].push_back(param.value.id);
        }
    }
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        for (u32 param_idx = 0; param_idx < block->params.size(); param_idx++) {
            if (is_exception_param(block_idx, param_idx)) continue;
            if (promoted_var_index.count(block->params[param_idx].name)) {
                all_promoted_value_ids[block->params[param_idx].name].push_back(
                    block->params[param_idx].value.id);
            }
        }
    }

    // 5c. Analyze block params before modification
    // Exception params are classified as non-promoted regardless of name match.
    Vector<BlockParamAnalysis> block_analyses(func->blocks.size());
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        BlockParamAnalysis& analysis = block_analyses[block_idx];
        analysis.original_params = block->params;
        for (u32 i = 0; i < block->params.size(); i++) {
            if (is_exception_param(block_idx, i)) {
                analysis.non_promoted_indices.push_back(i);
            } else if (promoted_var_index.count(block->params[i].name)) {
                analysis.promoted_indices.push_back(i);
            } else {
                analysis.non_promoted_indices.push_back(i);
            }
        }
    }

    // 5d. For EVERY block: prepend GetField loads for ALL promoted vars,
    //     remap all instructions and terminator using per-block remap.
    //     For catch blocks with exception params that match promoted vars,
    //     also insert SetField to store the exception value into the struct.
    //
    // The accessor each block creates per promoted var, indexed [block][pv_idx].
    // 5e consults it to recognize a write-back whose source is already the
    // field's own address, which would copy the field onto itself.
    Vector<Vector<ValueId>> block_accessors;
    block_accessors.resize(func->blocks.size());

    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        tsl::robin_map<u32, ValueId> local_remap;

        // For catch blocks: if the exception param name matches a promoted var,
        // store it to the struct field first so subsequent GetField loads see it.
        Vector<IRInst*> prepend_insts;

        if (exception_param_block_ids.count(block_idx) && !block->params.empty()) {
            BlockParam& exc_param = block->params[0];
            auto pv_it = promoted_var_index.find(exc_param.name);
            if (pv_it != promoted_var_index.end()) {
                const PromotedVar& pv = promoted_vars[pv_it->second];
                IRInst* store_inst = allocator.emplace<IRInst>();
                store_inst->op = IROp::SetField;
                store_inst->type = pv.type;
                store_inst->result = func->new_value_for(store_inst);
                store_inst->field.object = self_val;
                store_inst->field.field_name = pv.name;
                store_inst->field.slot_offset = pv.field_slot_offset;
                store_inst->field.slot_count = pv.field_slot_count;
                store_inst->store_value = exc_param.value;
                prepend_insts.push_back(store_inst);
            }
        }

        // Create per-block accessors for all promoted vars.
        //
        // A scalar var round-trips through the state field by value: GetField
        // here, SetField at each jump (5e). A **value struct** cannot — its SSA
        // value is the *address* of its storage, and loading a struct's slots
        // into a register would truncate it to the first slot. Such a var lives
        // inline in the state field instead, and the block reads its address
        // once with GetFieldAddr: field accesses and struct copies then operate
        // on the field itself, so mutations are already in the state and need
        // no write-back (5e skips them).
        block_accessors[block_idx].resize(promoted_vars.size());
        for (u32 pv_idx = 0; pv_idx < promoted_vars.size(); pv_idx++) {
            const PromotedVar& pv = promoted_vars[pv_idx];
            IRInst* inst = allocator.emplace<IRInst>();
            inst->op = is_inline_struct_var(pv.type) ? IROp::GetFieldAddr : IROp::GetField;
            inst->type = pv.type;
            inst->result = func->new_value_for(inst);
            inst->field.object = self_val;
            inst->field.field_name = pv.name;
            inst->field.slot_offset = pv.field_slot_offset;
            inst->field.slot_count = pv.field_slot_count;
            prepend_insts.push_back(inst);
            block_accessors[block_idx][pv_idx] = inst->result;

            // Map ALL original ValueIds for this promoted var to this block's load
            auto it = all_promoted_value_ids.find(pv.name);
            if (it != all_promoted_value_ids.end()) {
                for (u32 vid : it->second) {
                    local_remap[vid] = inst->result;
                }
            }
        }

        // Remap original instructions in-place
        for (auto* inst : block->instructions) {
            remap_inst_values(local_remap, inst);
        }
        // Remap terminator
        remap_terminator_values(local_remap, block->terminator);

        // Prepend GetField loads (and exception SetField if applicable) before original instructions
        Vector<IRInst*> new_insts;
        new_insts.reserve(prepend_insts.size() + block->instructions.size());
        for (auto* inst : prepend_insts) new_insts.push_back(inst);
        for (auto* inst : block->instructions) new_insts.push_back(inst);
        block->instructions = std::move(new_insts);
    }

    // 5e. For each jump edge with promoted args: insert SetField stores,
    //     then remove promoted args from the jump target.
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        Terminator& term = block->terminator;

        auto process_jump = [&](JumpTarget& target) {
            if (!target.block.is_valid()) return;
            u32 target_idx = target.block.id;
            if (target_idx >= block_analyses.size()) return;
            BlockParamAnalysis& target_analysis = block_analyses[target_idx];
            if (target_analysis.promoted_indices.empty()) return;

            // Insert SetField for each promoted arg (values already remapped in 5d)
            for (u32 pi : target_analysis.promoted_indices) {
                const BlockParam& param = target_analysis.original_params[pi];
                auto pv_it = promoted_var_index.find(param.name);
                assert(pv_it != promoted_var_index.end());
                const PromotedVar& pv = promoted_vars[pv_it->second];
                ValueId arg_value = target.args[pi].value;

                // An inline value struct is addressed, not held in a register,
                // so the write-back is a struct copy rather than a SetField
                // (which would store the *address* into a field sized for the
                // struct's slots). Only the entry block needs it: there the
                // argument is still the variable's original stack storage, so
                // the copy is what populates the state. Everywhere else the
                // argument has been remapped to this block's own accessor —
                // the field's address — and the copy would be a per-resume
                // memcpy of the field onto itself.
                if (is_inline_struct_var(pv.type)) {
                    ValueId accessor = block_idx < block_accessors.size()
                                           ? block_accessors[block_idx][pv_it->second]
                                           : ValueId::invalid();
                    if (arg_value != accessor) {
                        emit_set_struct_field(allocator, func, block, self_val,
                                              pv.name, pv.field_slot_offset, pv.field_slot_count,
                                              arg_value, pv.type);
                    }
                    continue;
                }

                emit_set_field(allocator, func, block, self_val, pv.name,
                               pv.field_slot_offset, pv.field_slot_count, arg_value, pv.type);
            }

            // Rebuild jump args keeping only non-promoted
            Vector<BlockArgPair> new_args;
            for (u32 npi : target_analysis.non_promoted_indices) {
                new_args.push_back(target.args[npi]);
            }
            target.args = alloc_span(allocator, new_args);
        };

        switch (term.kind) {
            case TerminatorKind::Goto:
                process_jump(term.goto_target);
                break;
            case TerminatorKind::Branch:
                process_jump(term.branch.then_target);
                process_jump(term.branch.else_target);
                break;
            default:
                break;
        }
    }

    // 5f. Remove promoted params from all blocks
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        BlockParamAnalysis& analysis = block_analyses[block_idx];
        if (analysis.promoted_indices.empty()) continue;
        IRBlock* block = func->blocks[block_idx];
        Vector<BlockParam> new_params;
        for (u32 npi : analysis.non_promoted_indices) {
            new_params.push_back(analysis.original_params[npi]);
        }
        block->params = std::move(new_params);
    }
}

// ===== Phase 2: Split at yield points, add dispatch =====

static void phase2_split(IRFunction* func, BumpAllocator& allocator,
                          ValueId self_val, Type* coro_yield_type,
                          const FieldInfo* state_field, const FieldInfo* yield_field,
                          TypeCache& types, const Vector<PromotedVar>& promoted_vars) {
    // Re-scan for yield points after Phase 1 (instruction indices changed)
    Vector<YieldPoint> yield_points;
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        for (u32 inst_idx = 0; inst_idx < block->instructions.size(); inst_idx++) {
            IRInst* inst = block->instructions[inst_idx];
            if (inst->op == IROp::Yield) {
                YieldPoint yp;
                yp.block_index = block_idx;
                yp.inst_index = inst_idx;
                yp.yielded_value = inst->unary;
                assert(block->terminator.kind == TerminatorKind::Goto);
                yp.resume_block_id = block->terminator.goto_target.block;
                yield_points.push_back(yp);
            }
        }
    }

    if (yield_points.empty()) return;

    // Build yield block lookup
    tsl::robin_map<u32, u32> block_to_yield_idx;
    for (u32 i = 0; i < yield_points.size(); i++) {
        block_to_yield_idx[yield_points[i].block_index] = i;
    }

    // 6a. Replace each yield with save-and-return
    for (u32 yi = 0; yi < yield_points.size(); yi++) {
        const YieldPoint& yp = yield_points[yi];
        IRBlock* block = func->blocks[yp.block_index];
        u32 yield_idx = yp.inst_index;

        Vector<IRInst*> new_insts;
        // Keep instructions before yield
        for (u32 i = 0; i < yield_idx; i++) {
            new_insts.push_back(block->instructions[i]);
        }
        // Skip the Yield instruction

        // SetField(__yield_val, yielded_value)
        IRInst* set_yield = allocator.emplace<IRInst>();
        set_yield->op = IROp::SetField;
        set_yield->type = coro_yield_type;
        set_yield->result = func->new_value_for(set_yield);
        set_yield->field.object = self_val;
        set_yield->field.field_name = yield_field->name;
        set_yield->field.slot_offset = yield_field->slot_offset;
        set_yield->field.slot_count = yield_field->slot_count;
        set_yield->store_value = yp.yielded_value;
        new_insts.push_back(set_yield);

        // SetField(__state, next_state)
        u32 next_state = yi + 1;
        IRInst* const_state = allocator.emplace<IRInst>();
        const_state->op = IROp::ConstInt;
        const_state->type = types.i32_type();
        const_state->result = func->new_value_for(const_state);
        const_state->const_data.int_val = static_cast<i64>(next_state);
        new_insts.push_back(const_state);

        IRInst* set_state = allocator.emplace<IRInst>();
        set_state->op = IROp::SetField;
        set_state->type = types.i32_type();
        set_state->result = func->new_value_for(set_state);
        set_state->field.object = self_val;
        set_state->field.field_name = state_field->name;
        set_state->field.slot_offset = state_field->slot_offset;
        set_state->field.slot_count = state_field->slot_count;
        set_state->store_value = const_state->result;
        new_insts.push_back(set_state);

        // Keep instructions after yield (Phase 1's SetField stores)
        for (u32 i = yield_idx + 1; i < block->instructions.size(); i++) {
            new_insts.push_back(block->instructions[i]);
        }

        // Load yield val and return
        IRInst* load_yield = allocator.emplace<IRInst>();
        load_yield->op = IROp::GetField;
        load_yield->type = coro_yield_type;
        load_yield->result = func->new_value_for(load_yield);
        load_yield->field.object = self_val;
        load_yield->field.field_name = yield_field->name;
        load_yield->field.slot_offset = yield_field->slot_offset;
        load_yield->field.slot_count = yield_field->slot_count;
        new_insts.push_back(load_yield);

        block->instructions = std::move(new_insts);
        finish_return(block, load_yield->result);
    }

    // 6b. Replace Return terminators with set-done + return-default
    for (auto* block : func->blocks) {
        if (block->terminator.kind != TerminatorKind::Return) continue;
        if (block_to_yield_idx.count(block->id.id)) continue;

        // Null-ify promoted noncopyable pointer fields before setting done state.
        // The IR builder's inline cleanup code has already freed the objects on this path,
        // but the struct fields still hold stale pointers. The destructor (called later
        // when the Coro goes out of scope) would double-free without this.
        for (const auto& pv : promoted_vars) {
            if (pv.type->is_copy()) continue;
            if (pv.type->kind == TypeKind::Uniq || pv.type->is_container() ||
                pv.type->is_coroutine()) {
                ValueId null_val = emit_const_null(allocator, func, block, types.nil_type());
                emit_set_field(allocator, func, block, self_val,
                               pv.name, pv.field_slot_offset, pv.field_slot_count,
                               null_val, pv.type);
            }
        }

        ValueId done_val = emit_const_int(allocator, func, block, CORO_STATE_DONE, types.i32_type());
        emit_set_field(allocator, func, block, self_val,
                       state_field->name, state_field->slot_offset, state_field->slot_count,
                       done_val, types.i32_type());
        // Return a default of the yield type. The value is never observed (done()
        // is true on this path). `ConstInt 0` is only valid C for scalar yield
        // types; for a struct yield, return the zero-initialized __yield_val field
        // (roxy_alloc zeroes the state struct) — a valid value of the right type.
        ValueId default_val = coro_yield_type->is_struct()
            ? emit_get_field(allocator, func, block, self_val, yield_field->name,
                             yield_field->slot_offset, yield_field->slot_count, coro_yield_type)
            : emit_const_int(allocator, func, block, 0, coro_yield_type);
        finish_return(block, default_val);
    }

    // 6c. Build dispatch block and chain
    u32 num_states = static_cast<u32>(yield_points.size()) + 1;
    BlockId original_entry = func->blocks[0]->id;
    u32 num_original_blocks = static_cast<u32>(func->blocks.size());

    // Create dispatch entry
    IRBlock* dispatch_entry = create_block(allocator, func, alloc_string(allocator, "dispatch"));
    ValueId state_loaded = emit_get_field(allocator, func, dispatch_entry, self_val,
                                           state_field->name, state_field->slot_offset,
                                           state_field->slot_count, types.i32_type());

    // Create trap block
    IRBlock* trap_block = create_block(allocator, func, alloc_string(allocator, "trap"));
    finish_unreachable(trap_block);

    // Build if-else chain
    IRBlock* current_dispatch = dispatch_entry;
    for (u32 i = 0; i < num_states; i++) {
        ValueId state_const = emit_const_int(allocator, func, current_dispatch, i, types.i32_type());
        ValueId is_match = emit_eq_i(allocator, func, current_dispatch,
                                      state_loaded, state_const, types.bool_type());

        BlockId target;
        if (i == 0) {
            target = original_entry;
        } else {
            target = yield_points[i - 1].resume_block_id;
        }

        if (i == num_states - 1) {
            finish_branch(current_dispatch, is_match, target, trap_block->id);
        } else {
            char name_buf[64];
            format_to(name_buf, sizeof(name_buf), "dispatch_{}", i + 1);
            IRBlock* next_dispatch = create_block(allocator, func, alloc_string(allocator, name_buf));
            finish_branch(current_dispatch, is_match, target, next_dispatch->id);
            current_dispatch = next_dispatch;
        }
    }

    // 6d. Rearrange blocks: dispatch+trap first, then original blocks
    Vector<IRBlock*> new_block_order;
    // Dispatch and trap blocks (appended after originals)
    for (u32 i = num_original_blocks; i < func->blocks.size(); i++) {
        new_block_order.push_back(func->blocks[i]);
    }
    // Original blocks
    for (u32 i = 0; i < num_original_blocks; i++) {
        new_block_order.push_back(func->blocks[i]);
    }

    // Build block ID remap
    tsl::robin_map<u32, u32> block_id_remap;
    for (u32 i = 0; i < new_block_order.size(); i++) {
        block_id_remap[new_block_order[i]->id.id] = i;
    }

    func->blocks = std::move(new_block_order);
    remap_all_block_ids(func, block_id_remap);

    // Renumber block IDs
    for (u32 i = 0; i < func->blocks.size(); i++) {
        func->blocks[i]->id = BlockId{i};
    }
}

// ===== Main lowering logic =====

static void lower_coroutine(IRFunction* original, IRModule* module,
                              BumpAllocator& allocator, TypeEnv& type_env) {
    TypeCache& types = type_env.types();
    Type* coro_yield_type = original->coro_yield_type;
    Type* coro_type = original->coro_type;

    // Step 1: Find all yield points
    Vector<YieldPoint> yield_points;
    for (u32 block_idx = 0; block_idx < original->blocks.size(); block_idx++) {
        IRBlock* block = original->blocks[block_idx];
        for (u32 inst_idx = 0; inst_idx < block->instructions.size(); inst_idx++) {
            IRInst* inst = block->instructions[inst_idx];
            if (inst->op == IROp::Yield) {
                YieldPoint yp;
                yp.block_index = block_idx;
                yp.inst_index = inst_idx;
                yp.yielded_value = inst->unary;
                assert(block->terminator.kind == TerminatorKind::Goto);
                yp.resume_block_id = block->terminator.goto_target.block;
                yield_points.push_back(yp);
            }
        }
    }

    // Step 2: Identify promoted variables from resume block parameters
    Vector<PromotedVar> promoted_vars;
    tsl::robin_map<StringView, u32> promoted_var_index;

    for (auto& yp : yield_points) {
        IRBlock* resume_block = original->blocks[yp.resume_block_id.id];
        for (auto& param : resume_block->params) {
            if (promoted_var_index.find(param.name) == promoted_var_index.end()) {
                PromotedVar pv;
                pv.name = param.name;
                pv.type = param.type;
                pv.field_slot_offset = 0;
                pv.field_slot_count = get_type_slot_count(param.type);
                promoted_var_index[param.name] = static_cast<u32>(promoted_vars.size());
                promoted_vars.push_back(pv);
            }
        }
    }

    // Step 3: Build the coroutine struct type.
    //
    // Field order is a runtime contract for first-class Coro<T> values:
    //   slot 0: __resume_idx (u32) — the resume function's dispatch index, read
    //           by CALL_INDIRECT exactly like a closure env's __call_idx, so an
    //           erased Coro<T> can `.resume()` without knowing its concrete type.
    //   slot 1: __state (i32) — at a fixed offset in every coroutine struct so
    //           `.done()` inlines a load+compare with no dispatch.
    // __yield_val, params, and promoted locals follow.
    u32 num_params = static_cast<u32>(original->params.size());
    Vector<FieldInfo> fields;
    u32 current_slot = 0;

    // __resume_idx
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__resume_idx");
        field.type = types.u32_type();
        field.is_pub = false;
        field.index = 0;
        field.slot_offset = current_slot;
        field.slot_count = 1;
        current_slot += 1;
        fields.push_back(field);
    }

    // __state
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__state");
        field.type = types.i32_type();
        field.is_pub = false;
        field.index = 1;
        field.slot_offset = current_slot;
        field.slot_count = 1;
        current_slot += 1;
        fields.push_back(field);
    }

    // __yield_val
    u32 yield_val_slot_count = get_type_slot_count(coro_yield_type);
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__yield_val");
        field.type = coro_yield_type;
        field.is_pub = false;
        field.index = 2;
        field.slot_offset = current_slot;
        field.slot_count = yield_val_slot_count;
        current_slot += yield_val_slot_count;
        fields.push_back(field);
    }

    // Index of the first parameter field (after __resume_idx, __state, __yield_val).
    const u32 first_param_field = 3;

    // Original function parameters
    for (u32 i = 0; i < num_params; i++) {
        FieldInfo field;
        field.name = original->params[i].name;
        field.type = original->params[i].type;
        field.is_pub = false;
        field.index = first_param_field + i;
        field.slot_offset = current_slot;
        field.slot_count = get_type_slot_count(original->params[i].type);
        current_slot += field.slot_count;
        fields.push_back(field);
    }

    // Promoted locals (skip those that share a name with a param)
    for (u32 i = 0; i < promoted_vars.size(); i++) {
        bool is_param = false;
        for (u32 p = 0; p < num_params; p++) {
            if (original->params[p].name == promoted_vars[i].name) {
                promoted_vars[i].field_slot_offset = fields[first_param_field + p].slot_offset;
                is_param = true;
                break;
            }
        }
        if (is_param) continue;

        promoted_vars[i].field_slot_offset = current_slot;
        FieldInfo field;
        field.name = promoted_vars[i].name;
        field.type = promoted_vars[i].type;
        field.is_pub = false;
        field.index = static_cast<u32>(fields.size());
        field.slot_offset = current_slot;
        field.slot_count = promoted_vars[i].field_slot_count;
        current_slot += field.slot_count;
        fields.push_back(field);
    }

    StringView struct_name = alloc_string_fmt(allocator, "__coro_{}", original->name);
    Type* struct_type = types.struct_type(struct_name, nullptr);
    struct_type->struct_info.fields = alloc_span(allocator, fields);
    struct_type->struct_info.slot_count = current_slot;
    struct_type->struct_info.constructors = Span<ConstructorInfo>();
    // Attach a synthetic default destructor so noncopyable() and cleanup recognize this struct
    DestructorInfo* dtor_info = reinterpret_cast<DestructorInfo*>(
        allocator.alloc_bytes(sizeof(DestructorInfo), alignof(DestructorInfo)));
    dtor_info->name = StringView();  // empty = default destructor
    dtor_info->param_types = Span<Type*>();
    dtor_info->decl = nullptr;
    struct_type->struct_info.destructors = Span<DestructorInfo>(dtor_info, 1);
    struct_type->struct_info.methods = Span<MethodInfo>();
    struct_type->struct_info.when_clauses = Span<WhenClauseInfo>();
    struct_type->struct_info.implemented_traits = Span<TraitImplRecord>();
    struct_type->struct_info.parent = nullptr;

    type_env.register_named_type(struct_name, struct_type);
    coro_type->coro_info.generated_struct_type = struct_type;

    // Make the synthesized state struct visible to the C backend, which drives
    // typedefs / forward decls / dependency sorting / TYPEID defines off
    // module->struct_types. collect_backend_types ran during IR building (before
    // coroutine lowering), so this struct isn't in that list yet. The VM /
    // bytecode path ignores struct_types, so this is safe for both pipelines.
    module->struct_types.push_back(struct_type);

    Type* uniq_struct_type = types.uniq_type(struct_type);
    Type* ref_struct_type = types.ref_type(struct_type);

    auto find_field = [&](StringView name) -> const FieldInfo* {
        for (auto& field : struct_type->struct_info.fields) {
            if (field.name == name) return &field;
        }
        return nullptr;
    };

    const FieldInfo* resume_idx_field = find_field(alloc_string(allocator, "__resume_idx"));
    const FieldInfo* state_field = find_field(alloc_string(allocator, "__state"));
    const FieldInfo* yield_field = find_field(alloc_string(allocator, "__yield_val"));

    // The resume function's name (this coroutine, transformed in place below).
    StringView resume_name = alloc_string_fmt(allocator, "__coro_{}$$resume", original->name);

    // ===== Generate init function =====
    IRFunction* init_func = allocator.emplace<IRFunction>();
    init_func->name = original->name;
    init_func->return_type = coro_type;

    // The init function inherits the coroutine's signature verbatim, including
    // `param_is_ptr`. That flag is the *caller's* calling convention: lowering
    // reads the callee's flags to decide whether an argument is passed as a
    // pointer or splatted into registers (`STRUCT_LOAD_REGS`). Dropping it made
    // every call site pass a pointer-convention parameter by value — a method
    // receiver (`self` is always is_ptr) or an `out`/`inout` argument — while
    // the callee still read it as a pointer. A `uniq` receiver survived that by
    // coincidence, since a pointer value is passed identically either way; a
    // stack struct receiver got its *contents* in the register and the RefInc
    // below dereferenced the first field as if it were an ObjectHeader.
    for (u32 i = 0; i < original->params.size(); i++) {
        const BlockParam& param = original->params[i];
        BlockParam new_param;
        new_param.value = init_func->new_value();
        new_param.type = param.type;
        new_param.name = param.name;
        init_func->params.push_back(new_param);
        init_func->param_is_ptr.push_back(
            i < original->param_is_ptr.size() ? original->param_is_ptr[i] : false);
    }

    IRBlock* init_entry = create_block(allocator, init_func, alloc_string(allocator, "entry"));
    ValueId obj = emit_new(allocator, init_func, init_entry, struct_name, uniq_struct_type);
    // Seed __resume_idx with the resume function's dispatch index so an erased
    // Coro<T> value can `.resume()` via CALL_INDIRECT (resolved at lowering).
    ValueId resume_idx = emit_func_index(allocator, init_func, init_entry, resume_name, types.u32_type());
    emit_set_field(allocator, init_func, init_entry, obj,
                   resume_idx_field->name, resume_idx_field->slot_offset, resume_idx_field->slot_count,
                   resume_idx, types.u32_type());
    ValueId zero = emit_const_int(allocator, init_func, init_entry, 0, types.i32_type());
    emit_set_field(allocator, init_func, init_entry, obj,
                   state_field->name, state_field->slot_offset, state_field->slot_count,
                   zero, types.i32_type());
    for (u32 i = 0; i < num_params; i++) {
        const FieldInfo* param_field = find_field(original->params[i].name);
        // A `ref` param stored into the state is a counted borrow held for the
        // coroutine's lifetime: acquire it here (released in $$delete). This keeps
        // the owner alive while the coro can still observe the borrow — deleting
        // the owner before the coro is destroyed traps.
        bool is_counted_borrow = param_field->type && param_field->type->kind == TypeKind::Ref;
        // A coroutine method's `self` is the one `ref` param that may not be
        // heap: `ref` proper can only borrow a heap object, but the receiver is
        // second-class (lifetimes.md, "The second-class family") and binds to
        // stack value-structs too. Counting a borrow of a stack struct would
        // write through `data - 8` into a neighbouring local, and the state
        // struct outlives the frame anyway, so the capture is unsound rather
        // than merely uncounted. Trap before anything writes through it —
        // closures guard the identical hazard the same way (see
        // `IRBuilder::emit_assert_heap` and closures.md, "self capture").
        if (is_counted_borrow && original->params[i].name == "self"_sv) {
            emit_unary_void_op(allocator, init_func, init_entry, IROp::AssertHeap,
                               init_func->params[i].value, types.void_type());
        }
        if (is_inline_struct_var(param_field->type)) {
            // By-value struct param: copy it into the inline state field.
            emit_set_struct_field(allocator, init_func, init_entry, obj,
                                  param_field->name, param_field->slot_offset,
                                  param_field->slot_count,
                                  init_func->params[i].value, param_field->type);
        } else {
            emit_set_field(allocator, init_func, init_entry, obj,
                           param_field->name, param_field->slot_offset, param_field->slot_count,
                           init_func->params[i].value, param_field->type);
        }
        if (is_counted_borrow) {
            emit_unary_void_op(allocator, init_func, init_entry, IROp::RefInc,
                               init_func->params[i].value, types.void_type());
        }
    }
    finish_return(init_entry, obj);

    // No $$done function is generated: `.done()` is inlined at every call site as
    // a load of __state (fixed slot 1) compared against CORO_STATE_DONE, so it
    // needs no dispatch and works uniformly on erased Coro<T> values.

    // Catch-clause variable names: a promoted `ref` catch param is a state field
    // set by exception dispatch, not a counted borrow, so the destructor must not
    // RefDec it (unlike ref params and ref locals). Collect the handler blocks'
    // exception-param names.
    tsl::robin_map<StringView, bool> catch_names;
    for (auto& handler : original->exception_handlers) {
        if (handler.handler_block.id < original->blocks.size()) {
            IRBlock* hb = original->blocks[handler.handler_block.id];
            if (!hb->params.empty()) catch_names[hb->params[0].name] = true;
        }
    }

    // ===== Generate destructor function =====
    IRFunction* dtor_func = generate_coro_destructor(allocator, struct_type,
                                                      original->name, types, module,
                                                      original->params, catch_names);

    // ===== Transform original into resume function =====
    ValueId self_val = original->new_value();

    // Phase 1: promote variables to struct fields (in-place)
    phase1_promote(original, allocator, promoted_vars, promoted_var_index,
                   self_val, ref_struct_type);

    // Phase 2: split at yields, add dispatch
    phase2_split(original, allocator, self_val, coro_yield_type,
                 state_field, yield_field, types, promoted_vars);

    // A promoted `ref` *local* is a counted borrow acquired mid-body (RefInc at
    // `var r = src`) and released on the resume path when its scope exits — the
    // IR builder pairs that RefDec with a Nullify. Promotion loads the local
    // from its state field (GetField) before the RefDec, so we clear the field
    // right after the RefDec with a SetField(null). Then the destructor's
    // null-guarded RefDec releases the borrow only when the coro is destroyed
    // while the local is still live (field non-null), never double-decrementing
    // a borrow already released on the resume/completion path. Ref *params* have
    // no mid-body RefDec (their per-frame inc/dec are suppressed), so this
    // touches only ref locals.
    {
        tsl::robin_map<StringView, bool> ref_local_fields;
        for (const auto& pv : promoted_vars) {
            if (pv.type && pv.type->kind == TypeKind::Ref && !catch_names.count(pv.name)) {
                ref_local_fields[pv.name] = true;
            }
        }
        if (!ref_local_fields.empty()) {
            for (auto* block : original->blocks) {
                tsl::robin_map<u32, IRInst*> defs;
                for (auto* inst : block->instructions) defs[inst->result.id] = inst;

                Vector<IRInst*> rebuilt;
                bool changed = false;
                for (auto* inst : block->instructions) {
                    rebuilt.push_back(inst);
                    if (inst->op != IROp::RefDec) continue;
                    auto dit = defs.find(inst->unary.id);
                    if (dit == defs.end() || dit->second->op != IROp::GetField) continue;
                    IRInst* def = dit->second;
                    if (!ref_local_fields.count(def->field.field_name)) continue;
                    // Clear the state field right after releasing the borrow.
                    IRInst* null_c = allocator.emplace<IRInst>();
                    null_c->op = IROp::ConstNull;
                    null_c->type = types.nil_type();
                    null_c->result = original->new_value_for(null_c);
                    rebuilt.push_back(null_c);
                    IRInst* clr = allocator.emplace<IRInst>();
                    clr->op = IROp::SetField;
                    clr->type = def->type;
                    clr->result = original->new_value_for(clr);
                    clr->field.object = def->field.object;
                    clr->field.field_name = def->field.field_name;
                    clr->field.slot_offset = def->field.slot_offset;
                    clr->field.slot_count = def->field.slot_count;
                    clr->store_value = null_c->result;
                    rebuilt.push_back(clr);
                    changed = true;
                }
                if (changed) block->instructions = std::move(rebuilt);
            }
        }
    }

    // Clean up stale cleanup_info for promoted variables.
    // After Phase 1, the original function's cleanup_info entries reference SSA values
    // and blocks that may be stale. Remove entries for promoted variables since their
    // cleanup is now handled by the destructor.
    {
        Vector<IRCleanupInfo> filtered_cleanup;
        for (u32 i = 0; i < original->cleanup_info.size(); i++) {
            const IRCleanupInfo& info = original->cleanup_info[i];
            bool is_promoted = false;
            for (const auto& pv : promoted_vars) {
                if (info.type == pv.type && pv.type->noncopyable()) {
                    is_promoted = true;
                    break;
                }
            }
            if (!is_promoted) {
                filtered_cleanup.push_back(info);
            }
        }
        original->cleanup_info = std::move(filtered_cleanup);
    }

    // Set resume function metadata
    original->name = alloc_string_fmt(allocator, "__coro_{}$$resume", init_func->name);
    original->return_type = coro_yield_type;
    original->is_coroutine = false;

    // ===== Replace in module =====
    for (u32 i = 0; i < module->functions.size(); i++) {
        if (module->functions[i] == original) {
            module->functions[i] = init_func;
            break;
        }
    }
    module->functions.push_back(original);
    module->functions.push_back(dtor_func);
}

void coroutine_lower(IRModule* module, BumpAllocator& allocator, TypeEnv& type_env) {
    Vector<IRFunction*> coroutine_funcs;
    for (auto* func : module->functions) {
        if (func->is_coroutine) {
            coroutine_funcs.push_back(func);
        }
    }
    for (auto* func : coroutine_funcs) {
        lower_coroutine(func, module, allocator, type_env);
    }
}

}

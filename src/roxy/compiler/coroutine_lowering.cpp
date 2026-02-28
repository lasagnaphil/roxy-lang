#include "roxy/compiler/coroutine_lowering.hpp"
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
    char tmp[256];
    format_to(tmp, sizeof(tmp), fmt, arg);
    return alloc_string(allocator, tmp);
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

static u32 get_type_slot_count(Type* type) {
    if (!type) return 0;
    switch (type->kind) {
        case TypeKind::Bool:
        case TypeKind::I8: case TypeKind::U8:
        case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Enum:
        case TypeKind::IntLiteral:
            return 1;
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F64:
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::List:
        case TypeKind::Map:
        case TypeKind::Coroutine:
            return 2;
        case TypeKind::Weak:
            return 4;  // 64-bit pointer + 64-bit generation
        case TypeKind::Struct:
            return type->struct_info.slot_count;
        case TypeKind::String:
            return 2;
        default:
            return 0;
    }
}

static IRInst* make_inst(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                          IROp op, Type* type) {
    IRInst* inst = allocator.emplace<IRInst>();
    inst->op = op;
    inst->type = type;
    inst->result = func->new_value();
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
        case IROp::StackAlloc: case IROp::VarAddr: case IROp::BlockArg:
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
        default:
            // Unary/binary ops
            inst->unary = remap_value(value_map, inst->unary);
            if (inst->op >= IROp::AddI && inst->op <= IROp::Shr) {
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
    auto remap_target = [&](JumpTarget& target) {
        auto it = block_map.find(target.block.id);
        if (it != block_map.end()) target.block.id = it->second;
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
    tsl::robin_map<StringView, Vector<u32>> all_promoted_value_ids;
    for (auto& param : old_params) {
        if (promoted_var_index.count(param.name)) {
            all_promoted_value_ids[param.name].push_back(param.value.id);
        }
    }
    for (auto* block : func->blocks) {
        for (auto& param : block->params) {
            if (promoted_var_index.count(param.name)) {
                all_promoted_value_ids[param.name].push_back(param.value.id);
            }
        }
    }

    // 5c. Analyze block params before modification
    Vector<BlockParamAnalysis> block_analyses(func->blocks.size());
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        BlockParamAnalysis& analysis = block_analyses[block_idx];
        analysis.original_params = block->params;
        for (u32 i = 0; i < block->params.size(); i++) {
            if (promoted_var_index.count(block->params[i].name)) {
                analysis.promoted_indices.push_back(i);
            } else {
                analysis.non_promoted_indices.push_back(i);
            }
        }
    }

    // 5d. For EVERY block: prepend GetField loads for ALL promoted vars,
    //     remap all instructions and terminator using per-block remap.
    for (u32 block_idx = 0; block_idx < func->blocks.size(); block_idx++) {
        IRBlock* block = func->blocks[block_idx];
        tsl::robin_map<u32, ValueId> local_remap;

        // Create GetField loads for all promoted vars
        Vector<IRInst*> prepend_insts;
        for (u32 pv_idx = 0; pv_idx < promoted_vars.size(); pv_idx++) {
            const PromotedVar& pv = promoted_vars[pv_idx];
            IRInst* inst = allocator.emplace<IRInst>();
            inst->op = IROp::GetField;
            inst->type = pv.type;
            inst->result = func->new_value();
            inst->field.object = self_val;
            inst->field.field_name = pv.name;
            inst->field.slot_offset = pv.field_slot_offset;
            inst->field.slot_count = pv.field_slot_count;
            prepend_insts.push_back(inst);

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

        // Prepend GetField loads before original instructions
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

                IRInst* inst = allocator.emplace<IRInst>();
                inst->op = IROp::SetField;
                inst->type = pv.type;
                inst->result = func->new_value();
                inst->field.object = self_val;
                inst->field.field_name = pv.name;
                inst->field.slot_offset = pv.field_slot_offset;
                inst->field.slot_count = pv.field_slot_count;
                inst->store_value = arg_value;
                block->instructions.push_back(inst);
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
                          TypeCache& types) {
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
        set_yield->result = func->new_value();
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
        const_state->result = func->new_value();
        const_state->const_data.int_val = static_cast<i64>(next_state);
        new_insts.push_back(const_state);

        IRInst* set_state = allocator.emplace<IRInst>();
        set_state->op = IROp::SetField;
        set_state->type = types.i32_type();
        set_state->result = func->new_value();
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
        load_yield->result = func->new_value();
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

        ValueId done_val = emit_const_int(allocator, func, block, CORO_STATE_DONE, types.i32_type());
        emit_set_field(allocator, func, block, self_val,
                       state_field->name, state_field->slot_offset, state_field->slot_count,
                       done_val, types.i32_type());
        ValueId default_val = emit_const_int(allocator, func, block, 0, coro_yield_type);
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

    // Step 3: Build the coroutine struct type
    u32 num_params = static_cast<u32>(original->params.size());
    Vector<FieldInfo> fields;
    u32 current_slot = 0;

    // __state
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__state");
        field.type = types.i32_type();
        field.is_pub = false;
        field.index = 0;
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
        field.index = 1;
        field.slot_offset = current_slot;
        field.slot_count = yield_val_slot_count;
        current_slot += yield_val_slot_count;
        fields.push_back(field);
    }

    // Original function parameters
    for (u32 i = 0; i < num_params; i++) {
        FieldInfo field;
        field.name = original->params[i].name;
        field.type = original->params[i].type;
        field.is_pub = false;
        field.index = 2 + i;
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
                promoted_vars[i].field_slot_offset = fields[2 + p].slot_offset;
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
    struct_type->struct_info.destructors = Span<DestructorInfo>();
    struct_type->struct_info.methods = Span<MethodInfo>();
    struct_type->struct_info.when_clauses = Span<WhenClauseInfo>();
    struct_type->struct_info.implemented_traits = Span<TraitImplRecord>();
    struct_type->struct_info.parent = nullptr;

    type_env.register_named_type(struct_name, struct_type);
    coro_type->coro_info.generated_struct_type = struct_type;

    Type* uniq_struct_type = types.uniq_type(struct_type);
    Type* ref_struct_type = types.ref_type(struct_type);

    auto find_field = [&](StringView name) -> const FieldInfo* {
        for (auto& field : struct_type->struct_info.fields) {
            if (field.name == name) return &field;
        }
        return nullptr;
    };

    const FieldInfo* state_field = find_field(alloc_string(allocator, "__state"));
    const FieldInfo* yield_field = find_field(alloc_string(allocator, "__yield_val"));

    // ===== Generate init function =====
    IRFunction* init_func = allocator.emplace<IRFunction>();
    init_func->name = original->name;
    init_func->return_type = coro_type;

    for (auto& param : original->params) {
        BlockParam new_param;
        new_param.value = init_func->new_value();
        new_param.type = param.type;
        new_param.name = param.name;
        init_func->params.push_back(new_param);
        init_func->param_is_ptr.push_back(false);
    }

    IRBlock* init_entry = create_block(allocator, init_func, alloc_string(allocator, "entry"));
    ValueId obj = emit_new(allocator, init_func, init_entry, struct_name, uniq_struct_type);
    ValueId zero = emit_const_int(allocator, init_func, init_entry, 0, types.i32_type());
    emit_set_field(allocator, init_func, init_entry, obj,
                   state_field->name, state_field->slot_offset, state_field->slot_count,
                   zero, types.i32_type());
    for (u32 i = 0; i < num_params; i++) {
        const FieldInfo* param_field = find_field(original->params[i].name);
        emit_set_field(allocator, init_func, init_entry, obj,
                       param_field->name, param_field->slot_offset, param_field->slot_count,
                       init_func->params[i].value, param_field->type);
    }
    finish_return(init_entry, obj);

    // ===== Generate done function =====
    StringView done_name = alloc_string_fmt(allocator, "__coro_{}$$done", original->name);
    IRFunction* done_func = allocator.emplace<IRFunction>();
    done_func->name = done_name;
    done_func->return_type = types.bool_type();

    BlockParam done_self;
    done_self.value = done_func->new_value();
    done_self.type = ref_struct_type;
    done_self.name = alloc_string(allocator, "self");
    done_func->params.push_back(done_self);
    done_func->param_is_ptr.push_back(false);

    IRBlock* done_entry = create_block(allocator, done_func, alloc_string(allocator, "entry"));
    ValueId done_state_val = emit_get_field(allocator, done_func, done_entry,
                                            done_self.value, state_field->name,
                                            state_field->slot_offset, state_field->slot_count,
                                            types.i32_type());
    ValueId done_sentinel = emit_const_int(allocator, done_func, done_entry, CORO_STATE_DONE, types.i32_type());
    ValueId is_done = emit_eq_i(allocator, done_func, done_entry,
                                 done_state_val, done_sentinel, types.bool_type());
    finish_return(done_entry, is_done);

    // ===== Transform original into resume function =====
    ValueId self_val = original->new_value();

    // Phase 1: promote variables to struct fields (in-place)
    phase1_promote(original, allocator, promoted_vars, promoted_var_index,
                   self_val, ref_struct_type);

    // Phase 2: split at yields, add dispatch
    phase2_split(original, allocator, self_val, coro_yield_type,
                 state_field, yield_field, types);

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
    module->functions.push_back(done_func);
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

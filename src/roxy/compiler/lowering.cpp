#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/ir_optimize.hpp"
#include "roxy/compiler/mangling.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/core/format.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/natives.hpp"

#include <cassert>
#include <cstdio>

namespace rx {

// Whether lowering this op can produce a nested call frame, so an exception
// raised inside it surfaces in this frame at the op's return address rather than
// at its own PC. Used to place a value's "ready" PC (see the lowering loop).
static bool op_may_push_frame(IROp op) {
    switch (op) {
        case IROp::Call:
        case IROp::CallNative:
        case IROp::CallExternal:
        case IROp::CallIndirect:
        case IROp::New:            // runs a constructor
            return true;
        default:
            return false;
    }
}

// Any IR comparison op (signed/unsigned integer, f32, f64). Deliberately
// broader than the currently fusable set (pick_fused in fuse_compare_branch):
// marking a compare that never fuses is harmless, and it means adding fused
// variants for a new family (e.g. unsigned) cannot silently skip the
// cross-block unfusable guard.
static bool is_comparison_op(IROp op) {
    switch (op) {
        case IROp::EqI: case IROp::NeI:
        case IROp::LtI: case IROp::LeI: case IROp::GtI: case IROp::GeI:
        case IROp::LtU: case IROp::LeU: case IROp::GtU: case IROp::GeU:
        case IROp::EqF: case IROp::NeF:
        case IROp::LtF: case IROp::LeF: case IROp::GtF: case IROp::GeF:
        case IROp::EqD: case IROp::NeD:
        case IROp::LtD: case IROp::LeD: case IROp::GtD: case IROp::GeD:
            return true;
        default:
            return false;
    }
}

BytecodeBuilder::BytecodeBuilder()
    : m_current_func(nullptr)
    , m_current_ir_func(nullptr)
    , m_next_reg(0)
    , m_next_stack_slot(0)
    , m_module(nullptr)
    , m_ir_module(nullptr)
    , m_has_error(false)
    , m_error(nullptr)
{}

void BytecodeBuilder::report_error(const char* message) {
    if (!m_has_error) {
        m_has_error = true;
        m_error = message;
    }
}

BCModule* BytecodeBuilder::build(IRModule* ir_module) {
    // Reset error state
    m_has_error = false;
    m_error = nullptr;

    m_ir_module = ir_module;

    // Use UniquePtr for automatic cleanup on error
    auto module = make_unique<BCModule>();
    m_module = module.get();
    m_module->name = ir_module->name;
    m_module->global_slot_count = ir_module->global_slot_count;

    // Build function name index map
    m_func_indices.clear();
    for (u32 i = 0; i < ir_module->functions.size(); i++) {
        m_func_indices[ir_module->functions[i]->name] = i;
    }

    // Build each function
    for (IRFunction* ir_func : ir_module->functions) {
        BCFunction* bc_func = build_function(ir_func);
        m_module->functions.push_back(UniquePtr<BCFunction>(bc_func));

        if (m_has_error) {
            m_module = nullptr;
            return nullptr;  // UniquePtr automatically cleans up
        }
    }

    m_module = nullptr;
    return module.release();  // Transfer ownership to caller
}

// Helper to compute the number of contiguous arg registers needed for a call instruction
template <typename F>
static u32 compute_call_arg_reg_count(IRInst* inst, IRFunction* callee_func,
                                       const Vector<Type*>& value_types,
                                       F get_struct_slot_count_fn) {
    u32 arg_reg_count = 0;
    bool is_external = (inst->op == IROp::CallExternal);
    u32 num_args = is_external ? inst->call_external.args.size() : inst->call.args.size();

    for (u32 i = 0; i < num_args; i++) {
        ValueId arg_val = is_external ? inst->call_external.args[i] : inst->call.args[i];
        bool param_is_ptr = (callee_func && i < callee_func->param_is_ptr.size() && callee_func->param_is_ptr[i]);

        if (param_is_ptr) {
            arg_reg_count += 1;
        } else {
            Type* arg_type = arg_val.id < value_types.size() ? value_types[arg_val.id] : nullptr;
            // Weak refs need 2 registers (pointer + generation)
            if (arg_type && arg_type->kind == TypeKind::Weak) {
                arg_reg_count += 2;
            } else {
                u32 arg_slot_count = get_struct_slot_count_fn(arg_type);
                if (arg_slot_count > 0 && arg_slot_count <= 4) {
                    arg_reg_count += (arg_slot_count + 1) / 2;
                } else {
                    arg_reg_count += 1;
                }
            }
        }
    }
    return arg_reg_count;
}

BCFunction* BytecodeBuilder::build_function(IRFunction* ir_func) {
    m_current_ir_func = ir_func;
    m_current_func = new BCFunction();
    m_current_func->name = ir_func->name;
    m_current_func->param_count = ir_func->params.size();

    // Compute ret_reg_count based on return type: 2 for weak refs, packed
    // slot-pair count for register-resident small structs, else 1.
    m_current_func->ret_reg_count = static_cast<u8>(get_value_reg_count(ir_func->return_type));

    // Reset state.
    // Dense ValueId-indexed side tables: refill with sentinels, keeping the
    // buffer (a direct-indexed vector reset, not a hashed clear — see profiling.md).
    u32 num_values = ir_func->next_value_id;
    m_value_to_reg.clear_keep_capacity();
    m_value_to_reg.reserve(num_values);
    m_value_types.clear_keep_capacity();
    m_value_types.reserve(num_values);
    m_value_ready_pcs.clear_keep_capacity();
    m_value_ready_pcs.reserve(num_values);
    for (u32 i = 0; i < num_values; i++) {
        m_value_to_reg.push_back(NO_REG);
        m_value_types.push_back(nullptr);
        m_value_ready_pcs.push_back(NO_READY_PC);
    }
    for (u32 i = 0; i < 256; i++) m_reg_to_value[i] = NO_VALUE;

    // BlockId-indexed code-offset table. Ids are dense [0, num_blocks) after RPO
    // reorder; refill with the NO_OFFSET sentinel, keeping the buffer.
    u32 num_blocks = ir_func->blocks.size();
    m_block_offsets.clear_keep_capacity();
    m_block_offsets.reserve(num_blocks);
    for (u32 i = 0; i < num_blocks; i++) m_block_offsets.push_back(NO_OFFSET);
    m_unfusable_cmp_pcs.clear();
    m_nullify_pcs.clear();
    m_ref_inc_pcs.clear();
    m_cleanup_kill_pcs.clear();
    // m_requires_register is rebuilt fresh by compute_const_use_modes().
    m_jump_patches.clear_keep_capacity();
    free_regs_reset();
    m_active.clear_keep_capacity();
    m_spill_slots.clear();
    m_delete_desc_cache.clear();
    m_has_spilling = false;
    m_scratch_regs[0] = m_scratch_regs[1] = 0xFF;
    m_next_reg = 0;
    m_next_stack_slot = 0;

    // Step 0: Compute unwind coverage for cleanup records (which blocks each
    // tracked value is owned in). Must precede compute_liveness — its Pass 5b
    // pins each tracked register across the whole coverage, so a covered block
    // laid out past the scope's normal exit cannot recycle it.
    compute_cleanup_coverage(ir_func);

    // Step 1: Compute liveness intervals for all SSA values
    // (blocks are already in RPO order from IR building)
    compute_liveness(ir_func);

    // Step 1b: Identify constants whose every use is RK-eligible. These don't
    // need a register or LOAD_INT/LOAD_CONST — the RK opcode reads them
    // directly from the constant pool. Skipping their LOAD removes one dispatch
    // per inner-loop constant (e.g. `2.0`, `4.0`, `1` in mandelbrot).
    compute_const_use_modes(ir_func);

    // Step 2: Allocate registers for function parameters (pre-colored)
    u8 param_reg_offset = 0;
    for (u32 i = 0; i < ir_func->params.size(); i++) {
        const auto& param = ir_func->params[i];

        // Map this parameter value to its starting register
        m_value_to_reg[param.value.id] = param_reg_offset;
        m_reg_to_value[param_reg_offset] = param.value.id;
        if (param.type) {
            m_value_types[param.value.id] = param.type;
        }

        // Check if this parameter is a pointer (out/inout)
        bool is_ptr_param = (i < ir_func->param_is_ptr.size() && ir_func->param_is_ptr[i]);

        // Calculate how many registers this parameter uses
        u8 reg_count = 1;
        if (!is_ptr_param) {
            reg_count = static_cast<u8>(get_value_reg_count(param.type));
        }

        // Add each register used by this parameter to the active set so it
        // can expire when no longer used
        if (param.value.id < m_live_ranges.size()) {
            u32 last_use = m_live_ranges[param.value.id].last_use_point;
            for (u8 r = 0; r < reg_count; r++) {
                insert_active(static_cast<u8>(param_reg_offset + r), last_use);
            }
        }

        param_reg_offset += reg_count;
    }
    m_next_reg = param_reg_offset;
    m_current_func->param_register_count = param_reg_offset;

    // Step 3: Liveness-aware pre-allocation of all SSA values
    {
        u32 alloc_point = 0;
        for (IRBlock* block : ir_func->blocks) {

            // Block parameters
            for (const auto& param : block->params) {
                expire_before(alloc_point);
                if (!has_register(param.value)) {
                    u32 reg_count = get_value_reg_count(param.type);
                    if (reg_count > 1) {
                        allocate_multi_register_value(param.value, reg_count);
                    } else {
                        allocate_register(param.value);
                    }
                }
                if (param.type) {
                    m_value_types[param.value.id] = param.type;
                }
                alloc_point++;
            }

            // Instructions
            for (IRInst* inst : block->instructions) {
                expire_before(alloc_point);

                bool is_call = (inst->op == IROp::Call || inst->op == IROp::CallNative ||
                                inst->op == IROp::CallExternal || inst->op == IROp::CallIndirect);

                if (inst->result.is_valid() && !has_register(inst->result)) {
                    // Skip register allocation for RK-only constants: the LOAD
                    // is also skipped in lower_instruction, and try_emit_rk_binary
                    // reads the value directly from the constant pool.
                    if (!is_skip_load_const(inst)) {
                        u32 reg_count = get_value_reg_count(inst->type);
                        if (is_call) {
                            // Calls allocate dst + their contiguous arg window
                            // together in reserve_call_window below (needs the
                            // per-op arg register count).
                        } else if (reg_count > 1) {
                            allocate_multi_register_value(inst->result, reg_count);
                        } else {
                            allocate_register(inst->result);
                        }
                    }
                }
                if (inst->result.is_valid() && inst->type) {
                    m_value_types[inst->result.id] = inst->type;
                }

                // For calls, reserve dst + contiguous registers for args and
                // struct/weak returns together (reserve_call_window).
                if (is_call && inst->op != IROp::CallIndirect && inst->result.is_valid()) {
                    // Compute actual arg register count based on types
                    StringView func_name = (inst->op == IROp::CallExternal)
                        ? inst->call_external.func_name : inst->call.func_name;
                    IRFunction* callee_func = nullptr;
                    auto func_it = m_func_indices.find(func_name);
                    if (func_it != m_func_indices.end()) {
                        callee_func = m_ir_module->functions[func_it->second];
                    }
                    u32 total_arg_regs = compute_call_arg_reg_count(inst, callee_func, m_value_types,
                        [this](Type* t) { return get_struct_slot_count(t); });

                    reserve_call_window(inst, call_return_extra_regs(inst->type), total_arg_regs);
                }

                // CallIndirect (closure dispatch): callee resolved at runtime, but the
                // arg register block uses the same convention as direct CALL.
                if (inst->op == IROp::CallIndirect && inst->result.is_valid()) {
                    // No callee_func at IR time — sum register counts from explicit-arg types.
                    u32 total_arg_regs = 0;
                    for (auto arg_id : inst->call_indirect.args) {
                        Type* arg_type = value_type_of(arg_id.id);
                        u32 arg_slot_count = get_struct_slot_count(arg_type);
                        if (arg_slot_count > 0 && arg_slot_count <= 4) {
                            total_arg_regs += (arg_slot_count + 1) / 2;
                        } else {
                            total_arg_regs += get_value_reg_count(arg_type);
                        }
                    }

                    reserve_call_window(inst, call_return_extra_regs(inst->type), total_arg_regs);
                }

                alloc_point++;
            }

            // Pre-allocate registers for forward-target block params at the terminator.
            // In RPO, forward-edge predecessors come before targets. The predecessor's
            // MOV writes to the target's block param register. If the param is only
            // allocated at its def point (in the target block), another value could
            // grab the same register between the MOV and the param's definition.
            expire_before(alloc_point);
            const Terminator& term = block->terminator;
            auto pre_alloc_target_params = [&](const JumpTarget& target) {
                if (!target.block.is_valid() || target.block.id >= ir_func->blocks.size()) return;
                IRBlock* target_block = ir_func->blocks[target.block.id];
                for (const auto& param : target_block->params) {
                    if (!has_register(param.value)) {
                        u32 reg_count = get_value_reg_count(param.type);
                        if (reg_count > 1) {
                            allocate_multi_register_value(param.value, reg_count);
                        } else {
                            allocate_register(param.value);
                        }
                    }
                    if (param.type) {
                        m_value_types[param.value.id] = param.type;
                    }
                }
            };
            switch (term.kind) {
                case TerminatorKind::Goto:
                    pre_alloc_target_params(term.goto_target);
                    break;
                case TerminatorKind::Branch:
                    pre_alloc_target_params(term.branch.then_target);
                    pre_alloc_target_params(term.branch.else_target);
                    break;
                default:
                    break;
            }

            // Terminator slot
            alloc_point++;
        }
    }

    // Emit function prologue: unpack struct parameters from registers to local stack
    // Track cumulative register offset to match the parameter allocation above
    u8 prologue_param_reg_offset = 0;
    for (u32 i = 0; i < ir_func->params.size(); i++) {
        const auto& param = ir_func->params[i];

        // Check if this parameter is a pointer (out/inout parameter)
        bool is_ptr_param = (i < ir_func->param_is_ptr.size() && ir_func->param_is_ptr[i]);

        u32 slot_count = get_struct_slot_count(param.type);

        // Calculate how many registers this parameter uses
        u8 reg_count = 1;
        if (!is_ptr_param && slot_count > 0 && slot_count <= 4) {
            reg_count = static_cast<u8>((slot_count + 1) / 2);
        }

        // Skip unpacking for pointer parameters - they already contain a pointer
        if (is_ptr_param) {
            prologue_param_reg_offset += reg_count;
            continue;
        }

        // Check if this type has a copy constructor (e.g., List<T> or Map<K,V> value params)
        // Skip copy for noncopyable containers — they use move semantics
        if (m_registry && param.type && param.type->is_container()) {
            if (param.type->noncopyable()) {
                prologue_param_reg_offset += reg_count;
                continue;
            }
            StringView copy_name = param.type->is_list()
                ? param.type->list_info.copy_native_name
                : param.type->map_info.copy_native_name;
            if (!copy_name.empty()) {
                i32 copy_fn_idx = m_registry->get_index(copy_name);
                if (copy_fn_idx >= 0) {
                    // Deep copy via native copy constructor call
                    // CALL_NATIVE convention: args at dst+1, dst+2, ...
                    u8 param_reg = prologue_param_reg_offset;
                    u8 copy_dst = bump_register();
                    u8 copy_arg = bump_register();  // = copy_dst + 1
                    emit_abc(Opcode::MOV, copy_arg, param_reg, 0);
                    // Two-word CALL_NATIVE: word 1 holds dst+arg_count, word 2 holds func_idx.
                    emit_abc(Opcode::CALL_NATIVE, copy_dst, 0, 1);
                    emit(static_cast<u32>(copy_fn_idx));
                    // Remap parameter to the copied value
                    m_value_to_reg[param.value.id] = copy_dst;
                    prologue_param_reg_offset += reg_count;
                    continue;
                }
            }
        }

        if (slot_count > 0 && slot_count <= 4) {
            // Small struct: param registers hold packed data
            // Allocate local stack space and unpack
            u32 stack_offset = m_next_stack_slot;
            m_next_stack_slot += slot_count;

            // Use the tracked register offset, not get_register() since we'll remap it
            u8 param_reg = prologue_param_reg_offset;

            // Get stack address into a new register
            u8 stack_ptr_reg = bump_register();
            emit_abi(Opcode::STACK_ADDR, stack_ptr_reg, static_cast<u16>(stack_offset));

            // Unpack registers to stack
            emit_abc(Opcode::STRUCT_STORE_REGS, stack_ptr_reg, param_reg, static_cast<u8>(slot_count));
            emit(0);  // Padding word

            // Remap the parameter value to the stack pointer register
            m_value_to_reg[param.value.id] = stack_ptr_reg;
        }
        else if (slot_count > 4) {
            // Large struct: param register holds pointer to caller's data
            // For value semantics, we need to copy to local stack
            u32 stack_offset = m_next_stack_slot;
            m_next_stack_slot += slot_count;

            u8 param_reg = prologue_param_reg_offset;  // Source pointer (caller's data)

            // Get local stack address into a new register
            u8 stack_ptr_reg = bump_register();
            emit_abi(Opcode::STACK_ADDR, stack_ptr_reg, static_cast<u16>(stack_offset));

            // Copy struct data from caller to local stack (value semantics)
            emit_abc(Opcode::STRUCT_COPY, stack_ptr_reg, param_reg, static_cast<u8>(slot_count));

            // Remap the parameter value to the local stack pointer register
            m_value_to_reg[param.value.id] = stack_ptr_reg;
        }

        prologue_param_reg_offset += reg_count;
    }

    // Second pass: emit bytecode
    for (IRBlock* block : ir_func->blocks) {

        // Record block offset
        m_block_offsets[block->id.id] = m_current_func->code.size();
        m_block_start_pc = m_current_func->code.size();
        m_call_use_pcs.clear();

        // Lower all instructions, recording where each value becomes readable.
        for (IRInst* inst : block->instructions) {
            lower_instruction(inst);
            if (inst->result.is_valid()) {
                // The register holds the value from the instruction's end onward
                // — except when the instruction pushes a frame. An exception
                // propagating out of a callee unwinds the caller at
                // `frame->pc`, which is exactly the CALL's *return address*, and
                // at that point the result register has not been written. So a
                // frame-pushing op is ready one PC later than it looks.
                u32 ready = static_cast<u32>(m_current_func->code.size());
                if (op_may_push_frame(inst->op)) ready += 1;
                if (inst->result.id < m_value_ready_pcs.size()) {
                    m_value_ready_pcs[inst->result.id] = ready;
                }
            }
        }

        // Lower terminator
        lower_terminator(block);
    }

    // Patch jump offsets
    patch_jumps();

    // Fuse compare + conditional branch pairs into single two-word instructions
    fuse_compare_branch();

    // Build exception handler table from IR exception handlers.
    //
    // Blocks in the try body are not guaranteed to be contiguous in the final
    // bytecode layout after reorder_blocks_rpo — e.g. a `while`'s body block
    // sits AFTER the loop's fall-through when the loop lives inside a try, so
    // a single `[try_start_pc, try_end_pc)` window would either miss the body
    // or over-approximate and accidentally catch past the try. Instead, group
    // the try-body blocks (from handler.try_body_blocks, which was populated
    // in creation order by gen_try_stmt and remapped by reorder_blocks_rpo)
    // into contiguous runs of layout positions, and emit one BCExceptionHandler
    // per run that shares the same handler_pc / type_id.
    for (const auto& ir_handler : ir_func->exception_handlers) {
        u32 handler_offset = block_offset(ir_handler.handler_block.id);
        if (handler_offset == NO_OFFSET) continue;

        // Resolve type_id once (same for every BCExceptionHandler we'll emit).
        u32 type_id = 0;
        if (!ir_handler.type_name.empty()) {
            auto type_it = m_type_indices.find(ir_handler.type_name);
            if (type_it != m_type_indices.end()) {
                type_id = type_it->second + 1;
            } else {
                Type* exc_type = m_type_env ? m_type_env->type_by_name(ir_handler.type_name) : nullptr;
                if (exc_type && exc_type->is_struct()) {
                    u16 type_idx = m_module->types.size();
                    u32 size_bytes = exc_type->struct_info.slot_count * 4;
                    m_module->types.push_back({ir_handler.type_name, size_bytes, exc_type->struct_info.slot_count});
                    m_type_indices[ir_handler.type_name] = type_idx;
                    type_id = type_idx + 1;
                }
            }
        }

        u8 exception_reg = 0;
        IRBlock* handler_block = ir_func->blocks[ir_handler.handler_block.id];
        if (!handler_block->params.empty()) {
            exception_reg = get_result_register(handler_block->params[0].value);
        }

        // Collect the (post-reorder) layout positions for all try-body blocks.
        // RPO remap makes each block's id equal to its position in ir_func->blocks,
        // so we can sort the IDs directly.
        Vector<u32> body_ids;
        body_ids.reserve(ir_handler.try_body_blocks.size());
        for (BlockId bid : ir_handler.try_body_blocks) {
            if (bid.is_valid() && bid.id < ir_func->blocks.size()) {
                body_ids.push_back(bid.id);
            }
        }
        if (body_ids.empty()) continue;
        // Insertion sort (counts are tiny — at most a few dozen blocks per try).
        for (u32 i = 1; i < body_ids.size(); i++) {
            u32 v = body_ids[i];
            u32 j = i;
            while (j > 0 && body_ids[j - 1] > v) {
                body_ids[j] = body_ids[j - 1];
                j--;
            }
            body_ids[j] = v;
        }

        // Walk sorted layout positions, collapsing runs of consecutive ids into a
        // single [run_start_pc, run_end_pc) window.
        u32 run_start = body_ids[0];
        u32 run_end = body_ids[0];
        auto emit_range = [&](u32 first_block_id, u32 last_block_id) {
            u32 start_offset = block_offset(first_block_id);
            if (start_offset == NO_OFFSET) return;
            BCExceptionHandler bc_handler;
            bc_handler.try_start_pc = start_offset;

            // End PC = offset of the block after the run's last block (or end of code).
            u32 after_block_id = last_block_id + 1;
            if (after_block_id < ir_func->blocks.size()) {
                u32 after_offset = block_offset(after_block_id);
                bc_handler.try_end_pc = (after_offset != NO_OFFSET)
                    ? after_offset
                    : m_current_func->code.size();
            } else {
                bc_handler.try_end_pc = m_current_func->code.size();
            }

            bc_handler.handler_pc = handler_offset;
            bc_handler.type_id = type_id;
            bc_handler.exception_reg = exception_reg;
            m_current_func->exception_handlers.push_back(bc_handler);
        };

        for (u32 i = 1; i < body_ids.size(); i++) {
            if (body_ids[i] == run_end + 1) {
                run_end = body_ids[i];
            } else {
                emit_range(run_start, run_end);
                run_start = body_ids[i];
                run_end = body_ids[i];
            }
        }
        emit_range(run_start, run_end);
    }

    // Build cleanup records from IR cleanup info
    for (u32 ci_index = 0; ci_index < ir_func->cleanup_info.size(); ci_index++) {
        const auto& ir_cleanup = ir_func->cleanup_info[ci_index];
        u32 start_offset = block_offset(ir_cleanup.start_block.id);
        u32 end_offset = block_offset(ir_cleanup.end_block.id);

        if (start_offset == NO_OFFSET || end_offset == NO_OFFSET) {
            continue;
        }

        BCCleanupRecord record;
        record.scope_start_pc = start_offset;

        // scope_end_pc is the offset AFTER the end block's last instruction —
        // except for a record that hands off at a merge, which ends exactly
        // where the merge block begins (see IRCleanupInfo::ends_before_block).
        BlockId end_block_id = ir_cleanup.end_block;
        if (ir_cleanup.ends_before_block) {
            record.scope_end_pc = end_offset;
        } else if (end_block_id.id + 1 < ir_func->blocks.size()) {
            u32 next_offset = block_offset(end_block_id.id + 1);
            if (next_offset != NO_OFFSET) {
                record.scope_end_pc = next_offset;
            } else {
                record.scope_end_pc = m_current_func->code.size();
            }
        } else {
            record.scope_end_pc = m_current_func->code.size();
        }

        // Narrow scope if a Nullify annotation marks an earlier ownership transfer
        auto nullify_it = m_nullify_pcs.find(ir_cleanup.value.id);
        if (nullify_it != m_nullify_pcs.end()) {
            u32 nullify_pc = nullify_it->second;
            if (nullify_pc >= record.scope_start_pc && nullify_pc < record.scope_end_pc) {
                record.scope_end_pc = nullify_pc;
            }
        }

        // A call-site receiver borrow is live only [RefInc, RefDec) around a
        // single call. Narrow scope_start to the RefInc — the block-derived
        // start would wrongly cover earlier-in-block argument evaluation, where
        // the borrow's register isn't yet initialized (lifetimes.md "Counting mechanics" / "Promotion").
        if (ir_cleanup.call_borrow) {
            auto inc_it = m_ref_inc_pcs.find(ir_cleanup.value.id);
            if (inc_it != m_ref_inc_pcs.end()) {
                u32 inc_pc = inc_it->second;
                if (inc_pc > record.scope_start_pc && inc_pc < record.scope_end_pc) {
                    record.scope_start_pc = inc_pc;
                }
            }
        }

        // An owned local's register holds nothing until the instruction that
        // produces it has run, but the block-derived start covers the whole
        // block — including a *call* that initializes it. A throw out of that
        // call would then unwind through a record naming an uninitialized
        // register.
        //
        // That was survivable while every tracked local was pointer-shaped:
        // registers start zeroed and a Delete of null is a no-op. A value struct
        // has no such null form — its register holds an address — so a stale
        // register (a live loop counter, say) got dereferenced as a struct.
        // Narrow the start to just past the defining instruction, the same
        // correction `call_borrow` already makes for a receiver borrow.
        record.live_start_pc = record.scope_start_pc;
        u32 ready_pc = ir_cleanup.value.id < m_value_ready_pcs.size()
            ? m_value_ready_pcs[ir_cleanup.value.id] : NO_READY_PC;
        if (ready_pc != NO_READY_PC && ready_pc > record.live_start_pc) {
            // Past the end means the value is never live anywhere in the
            // block-derived range, so the record describes nothing. That happens
            // when the register is only *made* to hold the value late in the
            // block — a small struct returns its slots packed in registers, and
            // the pointer the record names does not exist until the caller has
            // materialized them into stack storage. Firing there reads the raw
            // slot data as an address.
            if (ready_pc >= record.scope_end_pc) continue;
            record.live_start_pc = ready_pc;
        }

        // Map SSA value to register
        if (!has_register(ir_cleanup.value)) continue;
        record.register_idx = get_register(ir_cleanup.value);

        // RefDec / Unpin records release a borrow rather than destroy an owned
        // value, so they carry no delete descriptor.
        if (ir_cleanup.kind == IRCleanupKind::Unpin) {
            // Container element-borrow pin, released on unwind. Scope is the
            // [ContainerPin, Nullify) call window (narrowed above), like a
            // call-site ref borrow.
            record.kind = static_cast<u8>(BCCleanupKind::Unpin);
            record.delete_desc_idx = 0;
        } else if (ir_cleanup.kind == IRCleanupKind::StrRelease) {
            // Owned string local: release on the exception-unwind path (frees at
            // zero). Like RefDec it carries no delete descriptor; it keeps the
            // block-derived scope + Nullify narrowing (a string local's lifetime,
            // never whole-function like a ref param).
            record.kind = static_cast<u8>(BCCleanupKind::StrRelease);
            record.delete_desc_idx = 0;
        } else if (ir_cleanup.kind == IRCleanupKind::RefDec) {
            record.kind = static_cast<u8>(BCCleanupKind::RefDec);
            record.delete_desc_idx = 0;
            // A ref *parameter* is a borrow live for the entire function, so its
            // RefDec record spans the whole body [0, code.size()). The
            // block-derived scope used for owned locals is wrong here: functions
            // whose every path returns/throws have no single "end block" at the
            // max PC, which would truncate the range and skip throws past it.
            // Spanning the whole body means any escaping throw decrements the
            // borrow, while in-function handlers (handler_pc < code.size()) are
            // left to the normal-path RefDec via the "handler in scope" skip.
            // Ref *locals* keep the block-derived scope (+ Nullify narrowing
            // computed above), matching their actual lifetime.
            if (ir_cleanup.whole_function_scope) {
                record.scope_start_pc = 0;
                record.live_start_pc = 0;
                record.scope_end_pc = static_cast<u32>(m_current_func->code.size());
            }
        } else {
            record.kind = static_cast<u8>(BCCleanupKind::Delete);
            record.delete_desc_idx = build_delete_desc(ir_cleanup.type);
        }

        m_current_func->cleanup_records.push_back(record);

        // Extension records: the value's unwind coverage
        // (compute_cleanup_coverage) can include blocks laid out past the main
        // interval — RPO places a throw-terminated branch after the scope's
        // normal-exit block, so the Nullify-truncated interval above misses it
        // and a throw there leaked the value. Emit one extension record per
        // contiguous layout run of covered blocks, clipped to the part beyond
        // the main interval, truncated at the first kill PC inside a block.
        // Pass 5b pinned the register across the coverage, so reading it at
        // these PCs is safe. The unwinder evaluates head + extensions as one
        // group (see execute_cleanup).
        if (ci_index >= m_cleanup_covered_blocks.size()) continue;
        const Vector<u32>& covered = m_cleanup_covered_blocks[ci_index];
        if (covered.empty()) continue;

        auto first_kill_pc_in = [&](u32 range_start, u32 range_end) -> u32 {
            auto kill_it = m_cleanup_kill_pcs.find(ir_cleanup.value.id);
            if (kill_it == m_cleanup_kill_pcs.end()) return NO_OFFSET;
            u32 best = NO_OFFSET;
            for (u32 pc : kill_it->second) {
                if (pc >= range_start && pc <= range_end && pc < best) best = pc;
            }
            return best;
        };

        u32 run_start = NO_OFFSET;
        u32 run_end = NO_OFFSET;  // exclusive
        auto flush_run = [&]() {
            if (run_start != NO_OFFSET && run_end != NO_OFFSET && run_start < run_end) {
                // Only the part beyond the main interval is new coverage; the
                // part inside it is already covered, and anything before its
                // scope_start would predate the value's live range.
                u32 lo = run_start > record.scope_end_pc ? run_start : record.scope_end_pc;
                if (lo < run_end) {
                    BCCleanupRecord ext;
                    ext.scope_start_pc = lo;
                    ext.scope_end_pc = run_end;
                    ext.live_start_pc = lo;
                    ext.register_idx = record.register_idx;
                    ext.kind = record.kind;
                    ext.delete_desc_idx = record.delete_desc_idx;
                    ext.is_extension = true;
                    m_current_func->cleanup_records.push_back(ext);
                }
            }
            run_start = NO_OFFSET;
            run_end = NO_OFFSET;
        };

        u32 prev_block = 0;
        bool run_open = false;
        for (u32 covered_block : covered) {
            u32 b_start = block_offset(covered_block);
            if (b_start == NO_OFFSET) {
                flush_run();
                run_open = false;
                continue;
            }
            u32 next_off = block_offset(covered_block + 1);
            u32 b_end = next_off != NO_OFFSET ? next_off
                                              : static_cast<u32>(m_current_func->code.size());
            if (!run_open || covered_block != prev_block + 1) {
                flush_run();
                run_start = b_start;
            }
            u32 kill_pc = first_kill_pc_in(b_start, b_end);
            if (kill_pc != NO_OFFSET) {
                // Ownership ends inside this block: close the run at the kill
                // (successors of a kill block are not covered, so a
                // layout-adjacent covered block belongs to a fresh run).
                run_end = kill_pc;
                flush_run();
                run_open = false;
            } else {
                run_end = b_end;
                prev_block = covered_block;
                run_open = true;
            }
        }
        flush_run();
    }

    m_current_func->register_count = m_next_reg;
    m_current_func->local_stack_slots = m_next_stack_slot;
    return m_current_func;
}

u8 BytecodeBuilder::bump_register() {
    if (m_next_reg >= 255) {
        report_error("Register overflow: function uses too many values (max 255)");
        return 0xFF;
    }
    return static_cast<u8>(m_next_reg++);
}

void BytecodeBuilder::insert_active(u8 reg, u32 last_use) {
    ActiveAlloc alloc{last_use, reg};
    auto* pos = m_active.find_if([&](const ActiveAlloc& a) { return a.last_use > last_use; });
    if (pos) {
        m_active.insert(pos, alloc);
    } else {
        m_active.push_back(alloc);
    }
}

void BytecodeBuilder::allocate_multi_register_value(ValueId value, u32 reg_count) {
    u8 reg = bump_register();
    m_value_to_reg[value.id] = reg;
    m_reg_to_value[reg] = value.id;
    for (u32 r = 1; r < reg_count; r++) {
        u8 extra_reg = bump_register();
        if (extra_reg != 0xFF) m_reg_to_value[extra_reg] = value.id;
    }
    // Track every register in the active set: they expire like any other
    // value, and reserve_call_window's live floor must see them or a call
    // window would be placed on top of them. (Skipping this for
    // forward-target block params clobbered a loop-carried weak param when a
    // call in the loop body reused its "dead-looking" registers.)
    if (value.id < m_live_ranges.size()) {
        u32 last_use = m_live_ranges[value.id].last_use_point;
        for (u32 r = 0; r < reg_count; r++) {
            insert_active(static_cast<u8>(reg + r), last_use);
        }
    }
}

bool BytecodeBuilder::is_call_result(u32 value_id) const {
    if (!m_current_ir_func || value_id >= m_current_ir_func->values_by_id.size()) return false;
    IRInst* def = m_current_ir_func->values_by_id[value_id];
    if (!def) return false;
    return def->op == IROp::Call || def->op == IROp::CallNative ||
           def->op == IROp::CallExternal || def->op == IROp::CallIndirect;
}

void BytecodeBuilder::reserve_call_window(IRInst* inst, u32 extra_regs_for_return, u32 total_arg_regs) {
    ValueId result = inst->result;
    u32 dst_reg_count = get_value_reg_count(inst->type);
    if (dst_reg_count == 0) dst_reg_count = 1;
    // Window layout (interpreter CALL): dst at the base, multi-register
    // returns right above it, then the contiguous argument block
    // (first_arg = dst + ret_reg_count). One contiguous block anchored at dst.
    u32 block_size = extra_regs_for_return + 1 + total_arg_regs;

    u32 last_use = 0;
    if (result.id < m_live_ranges.size()) {
        last_use = m_live_ranges[result.id].last_use_point;
    }

    // Place the window at the lowest register above every live value: dead
    // space (expired values, earlier call windows) is reused continuously, so
    // call-dense functions no longer consume a fresh register per call and
    // long-lived call results settle at low registers instead of stranding at
    // an ever-rising frame top. Values allocated later may reuse in-window
    // registers from the free list — safe, because SSA ordering puts their
    // definitions after this call executes, and loop-carried values are block
    // params whose back-edge arg uses keep them in the active set here.
    // Inserting the dst registers into the active set (unlike the historical
    // bump-only path) lets them expire and return to the free list. Call
    // results still never *spill* — see spill_furthest.
    while (true) {
        u32 floor_reg = 0;
        for (u32 i = 0; i < m_active.size(); i++) {
            if (static_cast<u32>(m_active[i].reg) + 1 > floor_reg) {
                floor_reg = static_cast<u32>(m_active[i].reg) + 1;
            }
        }
        if (m_has_spilling) {
            // The scratch registers are permanently reserved for spill
            // reloads; arg MOVs may reload spilled args through them, so the
            // window must sit strictly above both.
            u32 scratch_top = static_cast<u32>(m_scratch_regs[0] > m_scratch_regs[1]
                                                   ? m_scratch_regs[0]
                                                   : m_scratch_regs[1]) + 1;
            if (scratch_top > floor_reg) floor_reg = scratch_top;
        }
        if (floor_reg + block_size <= 255) {
            u8 base = static_cast<u8>(floor_reg);
            m_value_to_reg[result.id] = base;
            m_reg_to_value[base] = result.id;
            // The dst register(s) may coincide with expired registers still
            // sitting in the free list — claim them exclusively until the
            // result dies, or a later value would allocate the same register
            // and clobber the live call result. The *arg* registers above dst
            // may stay in the free list: any value reusing one is defined
            // after this call executes (SSA ordering), when the transient arg
            // block is already dead.
            free_reg_clear_range(base, dst_reg_count);
            for (u32 r = 0; r < dst_reg_count; r++) {
                insert_active(static_cast<u8>(base + r), last_use);
            }
            ensure_register_window(static_cast<u16>(floor_reg + block_size));
            return;
        }
        // Doesn't fit above the live values — spill the furthest-living ones
        // until it does.
        u32 active_before = m_active.size();
        spill_furthest();
        if (m_active.size() >= active_before) break;  // nothing spillable left
    }
    report_error("Register overflow: function uses too many values (max 255)");
    m_value_to_reg[result.id] = 0;  // dampen cascading errors downstream
}

void BytecodeBuilder::ensure_register_window(u16 needed_regs) {
    while (m_next_reg < needed_regs) {
        u16 before = m_next_reg;
        bump_register();
        // bump_register caps at 255 and reports an error without advancing;
        // stop here so a >255-register call window doesn't spin forever.
        if (m_next_reg == before) break;
    }
}

u8 BytecodeBuilder::allocate_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    if (m_value_to_reg[value.id] != NO_REG) {
        return static_cast<u8>(m_value_to_reg[value.id]);
    }

    // Determine if this value can reuse a freed register.
    // Cross-block values must always get fresh registers because the IR may have
    // partially-defined values (e.g., AND/OR short-circuit) where a value is only
    // defined on one branch. Fresh registers are zero-initialized by the VM.
    bool can_reuse = (value.id < m_value_same_block.size() && m_value_same_block[value.id]);

    u8 reg;
    if (can_reuse && !free_regs_empty()) {
        // Smallest available register, for deterministic allocation.
        reg = free_reg_take_min();
    } else {
        // Check if we need to spill before bumping
        u16 reg_limit = m_has_spilling ? static_cast<u16>(m_scratch_regs[0]) : 255;
        if (m_next_reg >= reg_limit && !free_regs_empty()) {
            // Free list has entries (from spilling cross-block values we can't normally reuse)
            reg = free_reg_take_min();
        } else if (m_next_reg >= reg_limit) {
            // No free registers and at the limit — spill to free one
            spill_furthest();
            // After spilling, there should be a register in the free list
            if (free_regs_empty()) {
                report_error("Internal error: spilling failed to free a register");
                return 0xFF;
            }
            reg = free_reg_take_min();
        } else {
            reg = bump_register();
        }
    }

    m_value_to_reg[value.id] = reg;
    m_reg_to_value[reg] = value.id;

    // Add to active set for expiry tracking.
    // With RPO ordering, liveness is correct for all values including block params.
    // Block params still get fresh registers (can_reuse = false because they're
    // cross-block), but they ARE freed when dead.
    if (value.id < m_live_ranges.size()) {
        insert_active(reg, m_live_ranges[value.id].last_use_point);
    }

    return reg;
}

u8 BytecodeBuilder::get_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    u16 reg = m_value_to_reg[value.id];
    if (reg == NO_REG) {
        report_error("Internal error: SSA value used before allocation");
        return 0xFF;
    }
    return static_cast<u8>(reg);
}

bool BytecodeBuilder::has_register(ValueId value) const {
    if (!value.is_valid()) return false;
    return value.id < m_value_to_reg.size() && m_value_to_reg[value.id] != NO_REG;
}

void BytecodeBuilder::spill_furthest() {
    // Helper to compute spill slot size for a value
    auto spill_slot_size = [this](u32 value_id) -> u32 {
        Type* value_type = value_type_of(value_id);
        return (value_type && value_type->kind == TypeKind::Weak) ? 4 : 2;
    };

    // Pick the furthest-living *spillable* active entry and remove it.
    // Not spillable:
    //  - Call results: the CALL's argument window is anchored at the result
    //    register (first_arg = dst + ret_reg_count), so a spilled dst would
    //    re-anchor the window onto the scratch register and clobber live
    //    registers.
    //  - Multi-register values (weak refs, register-resident small structs)
    //    and unowned second-reg entries: the spill/reload bookkeeping tracks
    //    a single register per value, so evicting part of a pair would free a
    //    live register without saving it.
    // Returns false when nothing spillable remains (caller reports the error).
    auto take_furthest_spillable = [this](ActiveAlloc& out) -> bool {
        for (u32 i = m_active.size(); i > 0; i--) {
            u32 idx = i - 1;
            u32 val = m_reg_to_value[m_active[idx].reg];
            if (val == NO_VALUE) continue;
            if (is_call_result(val)) continue;
            Type* value_type = value_type_of(val);
            if (value_type && get_value_reg_count(value_type) > 1) continue;
            out = m_active[idx];
            for (u32 j = idx + 1; j < m_active.size(); j++) m_active[j - 1] = m_active[j];
            m_active.pop_back();
            return true;
        }
        return false;
    };

    // First time: reserve 2 scratch registers by spilling the 2 furthest-living values
    if (!m_has_spilling) {
        m_has_spilling = true;
        for (int s = 0; s < 2; s++) {
            ActiveAlloc furthest;
            if (!take_furthest_spillable(furthest)) {
                report_error("Internal error: no active values to spill for scratch registers");
                return;
            }

            u32 spilled_val = m_reg_to_value[furthest.reg];
            u32 slot_size = 2;
            if (spilled_val != NO_VALUE) {
                slot_size = spill_slot_size(spilled_val);
                u32 spill_slot = m_next_stack_slot;
                m_next_stack_slot += slot_size;
                m_spill_slots[spilled_val] = spill_slot;
                m_value_to_reg[spilled_val] = NO_REG;
                m_reg_to_value[furthest.reg] = NO_VALUE;
            } else {
                m_next_stack_slot += 2;
            }

            m_scratch_regs[s] = furthest.reg;
            // Scratch regs are NOT added to free list — they're permanently reserved
        }
        // Ensure scratch_regs[0] < scratch_regs[1] for consistent ordering
        if (m_scratch_regs[0] > m_scratch_regs[1]) {
            u8 tmp = m_scratch_regs[0];
            m_scratch_regs[0] = m_scratch_regs[1];
            m_scratch_regs[1] = tmp;
        }
    }

    // Spill one more value to free a register for the caller
    ActiveAlloc furthest;
    if (!take_furthest_spillable(furthest)) {
        report_error("Internal error: no active values to spill");
        return;
    }

    u32 spilled_val = m_reg_to_value[furthest.reg];
    if (spilled_val != NO_VALUE) {
        u32 slot_size = spill_slot_size(spilled_val);
        u32 spill_slot = m_next_stack_slot;
        m_next_stack_slot += slot_size;
        m_spill_slots[spilled_val] = spill_slot;
        m_value_to_reg[spilled_val] = NO_REG;
        m_reg_to_value[furthest.reg] = NO_VALUE;
    } else {
        m_next_stack_slot += 2;
    }

    free_reg_add(furthest.reg);
}

u8 BytecodeBuilder::get_result_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    if (m_value_to_reg[value.id] != NO_REG) return static_cast<u8>(m_value_to_reg[value.id]);

    // Spilled result: compute into scratch[0], will be spilled after
    if (m_spill_slots.count(value.id)) return m_scratch_regs[0];

    report_error("Internal error: SSA value has no register or spill slot");
    return 0xFF;
}

u8 BytecodeBuilder::ensure_in_register(ValueId value, u8 scratch_index) {
    if (!value.is_valid()) return 0xFF;

    if (m_value_to_reg[value.id] != NO_REG) return static_cast<u8>(m_value_to_reg[value.id]);

    auto spill_it = m_spill_slots.find(value.id);
    if (spill_it != m_spill_slots.end()) {
        u8 scratch = m_scratch_regs[scratch_index];
        emit_abi(Opcode::RELOAD_REG, scratch, static_cast<u16>(spill_it->second));

        // If this is a weak value (2 registers), also reload the second register
        Type* value_type = value_type_of(value.id);
        if (value_type && value_type->kind == TypeKind::Weak) {
            emit_abi(Opcode::RELOAD_REG, scratch + 1, static_cast<u16>(spill_it->second + 2));
        }

        return scratch;
    }

    // Skip-load constants (compute_const_use_modes) normally never touch a
    // register — every use reads them from an RK slot. Two paths still land
    // here needing one in a register: an RK op whose *other* operand is also a
    // constant (only one can take the K slot), and an RK bail-out when the
    // constant-pool index outgrows the 8-bit RK field. Materialize on demand
    // through the normal allocator, at the use point.
    IRInst* def = nullptr;
    if (m_current_ir_func && value.id < m_current_ir_func->values_by_id.size()) {
        def = m_current_ir_func->values_by_id[value.id];
    }
    if (def && (def->op == IROp::ConstInt || def->op == IROp::ConstF || def->op == IROp::ConstD)) {
        // Materialization happens at *emission* time, after all allocation
        // decisions — the free list here reflects end-of-function state, so a
        // freed register may still be owned at this program point. A fresh
        // bump_register() is the only always-safe choice: it sits above every
        // value and every call window.
        u8 reg = bump_register();
        if (reg != 0xFF) {
            m_value_to_reg[value.id] = reg;
            emit_const_load(def, reg);
        }
        return reg;
    }

    report_error("Internal error: SSA value used before allocation");
    return 0xFF;
}

void BytecodeBuilder::emit_const_load(IRInst* const_def, u8 dst) {
    switch (const_def->op) {
        case IROp::ConstInt: {
            i64 value = const_def->const_data.int_val;
            if (value >= IMM16_MIN && value <= IMM16_MAX) {
                emit_abi(Opcode::LOAD_INT, dst, static_cast<u16>(static_cast<i16>(value)));
            } else {
                u16 const_idx = add_constant(BCConstant::make_int(value));
                emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            }
            break;
        }
        case IROp::ConstF: {
            // Get f32 bit pattern and emit as LOAD_INT
            f32 fval = const_def->const_data.f32_val;
            u32 bits;
            memcpy(&bits, &fval, sizeof(bits));
            if (bits <= 0x7FFF) {
                // Small positive value - use immediate
                emit_abi(Opcode::LOAD_INT, dst, static_cast<u16>(bits));
            } else {
                // Use constant pool
                u16 const_idx = add_constant(BCConstant::make_int(static_cast<i64>(bits)));
                emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            }
            break;
        }
        case IROp::ConstD: {
            u16 const_idx = add_constant(BCConstant::make_float(const_def->const_data.f64_val));
            emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            break;
        }
        default:
            report_error("Internal error: emit_const_load on a non-constant definition");
            break;
    }
}

void BytecodeBuilder::spill_if_needed(ValueId value, u8 reg) {
    if (!value.is_valid()) return;

    auto spill_it = m_spill_slots.find(value.id);
    if (spill_it != m_spill_slots.end()) {
        emit_abi(Opcode::SPILL_REG, reg, static_cast<u16>(spill_it->second));

        // If this is a weak value (2 registers), also spill the second register
        Type* value_type = value_type_of(value.id);
        if (value_type && value_type->kind == TypeKind::Weak) {
            emit_abi(Opcode::SPILL_REG, reg + 1, static_cast<u16>(spill_it->second + 2));
        }
    }
}

void BytecodeBuilder::canonicalize_u32(IRInst* inst, u8 reg) {
    if (reg == 0xFF) return;
    if (inst->type && inst->type->kind == TypeKind::U32) {
        emit_abc(Opcode::TRUNC_U, reg, reg, 32);
    }
}

// Helper to update last_use_point for a value
static void mark_use(Vector<LiveRange>& live_ranges, ValueId value, u32 point) {
    if (!value.is_valid()) return;
    if (value.id < live_ranges.size()) {
        if (point > live_ranges[value.id].last_use_point) {
            live_ranges[value.id].last_use_point = point;
        }
    }
}

void BytecodeBuilder::compute_const_use_modes(IRFunction* ir_func) {
    // Mark every value that has at least one use requiring a register. A
    // ConstInt/ConstF/ConstD value not so marked is skip-load eligible — its
    // LOAD is not emitted and no register is allocated (see is_skip_load_const,
    // which reads this dense flag directly; §3.8, no separate skip-load set).
    //
    // Operand-position rules:
    //   - Binary op with RK variant + commutative → both positions skippable
    //     (canonicalization may swap to put constant on RHS)
    //   - Binary op with RK variant + non-commutative → only RHS skippable
    //   - All other op positions → require register
    u32 num_values = ir_func->next_value_id;
    m_requires_register.clear_keep_capacity();
    m_requires_register.reserve(num_values);
    for (u32 i = 0; i < num_values; i++) m_requires_register.push_back(false);

    auto mark_reg = [&](ValueId v) {
        if (v.is_valid() && v.id < num_values) m_requires_register[v.id] = true;
    };

    for (IRBlock* block : ir_func->blocks) {
        for (IRInst* inst : block->instructions) {
            // RK-eligible binary ops (exactly the set rk_opcode_for maps): the
            // RHS can land in the RK constant slot, and commutative
            // canonicalization can swap a LHS constant over — so only a
            // non-commutative LHS forces a register.
            if (rk_opcode_for(inst->op) != Opcode::NOP) {
                if (!is_commutative_binary(inst->op)) {
                    mark_reg(inst->binary.left);
                }
                continue;
            }
            // Every other op reads its operands from registers.
            for_each_operand(inst, [&](ValueId& operand) { mark_reg(operand); });
        }

        // Terminator operands — all require registers (Branch condition, Return
        // value, Goto block-arg passes are handled via register-to-register MOVs).
        for_each_terminator_operand(block->terminator,
                                    [&](ValueId& operand) { mark_reg(operand); });
    }

    // Skip-load eligibility (numeric Const* with no register-requiring use) is
    // now derived on demand from m_requires_register by is_skip_load_const() —
    // no separate collection pass or set to populate (§3.8).
}

// Compute, per cleanup record, the set of blocks in which the record's value is
// owned on the way to a potential throw: every block reachable from the
// record's start block without passing an ownership-ending kill. A kill is a
// Nullify annotation on the value, or a lowered cleanup op on it (Delete /
// StrRelease / RefDec) — the latter matters for the paths that end ownership
// without a Nullify (e.g. a temp adopted by a declaration).
//
// Exception edges are followed selectively. A throw inside a try transfers
// control to its handler block, and the value is owned there exactly when the
// throw site was covered — but whether the *handler* should be covered depends
// on who cleans up afterwards:
//   - If any try-body block kills the value, ownership at the handler is
//     path-dependent (a throw before vs. after the kill), which a PC-interval
//     record cannot express. Don't cover the handler: pre-kill throws fire
//     their covered records during the unwind, post-kill throws fire nothing.
//   - Otherwise cover the handler iff its normal continuation kills the value
//     (RAII: the builder cleans up on every normal exit of a scope the value is
//     live in), or the handler has no normal exit at all (a finally's catch-all
//     rethrows — the value must survive into the finally body and is freed by
//     its own covered record at the rethrow PC).
// Covering the handler serves the unwinder's handler-in-scope test: a handler
// inside the value's coverage means normal-path cleanup will run, so the
// unwind must not also fire (see execute_cleanup in interpreter.cpp).
void BytecodeBuilder::compute_cleanup_coverage(IRFunction* ir_func) {
    m_cleanup_covered_blocks.clear();
    u32 record_count = ir_func->cleanup_info.size();
    if (record_count == 0) return;
    for (u32 i = 0; i < record_count; i++) m_cleanup_covered_blocks.push_back({});

    u32 num_blocks = ir_func->blocks.size();
    if (num_blocks == 0) return;

    auto record_eligible = [](const IRCleanupInfo& info) {
        if (!info.value.is_valid() || !info.start_block.is_valid()) return false;
        if (info.whole_function_scope) return false;  // already spans [0, end)
        if (info.call_borrow) return false;           // sub-block [RefInc, Nullify) window
        if (info.kind == IRCleanupKind::Unpin) return false;
        return true;
    };

    // Seed m_cleanup_kill_pcs keys for eligible values, so the Delete /
    // StrRelease / RefDec / Nullify lowering paths append kill PCs only for
    // values a record tracks.
    bool any_eligible = false;
    for (const auto& info : ir_func->cleanup_info) {
        if (record_eligible(info)) {
            m_cleanup_kill_pcs[info.value.id];  // default-construct empty vector
            any_eligible = true;
        }
    }
    if (!any_eligible) return;

    // One IR walk: per tracked value, the blocks containing a kill of it.
    tsl::robin_map<u32, Vector<u32>> kill_blocks;
    for (IRBlock* block : ir_func->blocks) {
        for (IRInst* inst : block->instructions) {
            switch (inst->op) {
                case IROp::Nullify:
                case IROp::Delete:
                case IROp::StrRelease:
                case IROp::RefDec: {
                    if (!inst->unary.is_valid()) break;
                    if (m_cleanup_kill_pcs.find(inst->unary.id) == m_cleanup_kill_pcs.end()) break;
                    Vector<u32>& blocks = kill_blocks[inst->unary.id];
                    if (blocks.empty() || blocks.back() != block->id.id) {
                        blocks.push_back(block->id.id);
                    }
                    break;
                }
                default:
                    break;
            }
        }
    }
    auto block_has_kill = [&](u32 value_id, u32 block_id) {
        auto it = kill_blocks.find(value_id);
        if (it == kill_blocks.end()) return false;
        for (u32 b : it->second) {
            if (b == block_id) return true;
        }
        return false;
    };

    // Which handlers guard each block (index into ir_func->exception_handlers).
    Vector<Vector<u32>> block_to_handlers;
    block_to_handlers.reserve(num_blocks);
    for (u32 i = 0; i < num_blocks; i++) block_to_handlers.push_back({});
    for (u32 h = 0; h < ir_func->exception_handlers.size(); h++) {
        for (BlockId bid : ir_func->exception_handlers[h].try_body_blocks) {
            if (bid.is_valid() && bid.id < num_blocks) {
                block_to_handlers[bid.id].push_back(h);
            }
        }
    }

    auto for_each_successor = [&](u32 block_id, auto&& fn) {
        const Terminator& term = ir_func->blocks[block_id]->terminator;
        switch (term.kind) {
            case TerminatorKind::Goto:
                fn(term.goto_target.block);
                break;
            case TerminatorKind::Branch:
                fn(term.branch.then_target.block);
                fn(term.branch.else_target.block);
                break;
            default:
                break;
        }
    };

    // Scratch buffers shared across records.
    Vector<bool> visited;
    Vector<u32> worklist;
    Vector<bool> handler_reach;

    // Decide whether handler h's block continues value_id's coverage.
    auto handler_covered = [&](u32 h, u32 value_id) {
        const IRExceptionHandler& handler = ir_func->exception_handlers[h];
        if (!handler.handler_block.is_valid() || handler.handler_block.id >= num_blocks) {
            return false;
        }
        // (a) A kill anywhere in the try body makes ownership at the handler
        // path-dependent — don't cover.
        for (BlockId bid : handler.try_body_blocks) {
            if (bid.is_valid() && bid.id < num_blocks && block_has_kill(value_id, bid.id)) {
                return false;
            }
        }
        // (b)/(c) One BFS over the handler's normal continuation: covered iff a
        // kill of the value is reachable, or no Return is (rethrow-only path).
        handler_reach.clear_keep_capacity();
        handler_reach.reserve(num_blocks);
        for (u32 i = 0; i < num_blocks; i++) handler_reach.push_back(false);
        worklist.clear_keep_capacity();
        worklist.push_back(handler.handler_block.id);
        handler_reach[handler.handler_block.id] = true;
        bool saw_return = false;
        while (!worklist.empty()) {
            u32 b = worklist.back();
            worklist.pop_back();
            if (block_has_kill(value_id, b)) return true;
            if (ir_func->blocks[b]->terminator.kind == TerminatorKind::Return) saw_return = true;
            for_each_successor(b, [&](BlockId succ) {
                if (succ.is_valid() && succ.id < num_blocks && !handler_reach[succ.id]) {
                    handler_reach[succ.id] = true;
                    worklist.push_back(succ.id);
                }
            });
        }
        return !saw_return;
    };

    for (u32 ci = 0; ci < record_count; ci++) {
        const IRCleanupInfo& info = ir_func->cleanup_info[ci];
        if (!record_eligible(info)) continue;
        if (info.start_block.id >= num_blocks) continue;

        visited.clear_keep_capacity();
        visited.reserve(num_blocks);
        for (u32 i = 0; i < num_blocks; i++) visited.push_back(false);

        Vector<u32>& covered = m_cleanup_covered_blocks[ci];
        Vector<u32> pending;
        pending.push_back(info.start_block.id);
        // Memoized handler decisions for this record (handler index -> covered).
        tsl::robin_map<u32, bool> handler_memo;
        while (!pending.empty()) {
            u32 b = pending.back();
            pending.pop_back();
            if (b >= num_blocks || visited[b]) continue;
            // A record that hands off at a merge ends where the merge block
            // begins — the merge param's own record covers from there.
            if (info.ends_before_block && info.end_block.is_valid() && b == info.end_block.id) {
                continue;
            }
            visited[b] = true;
            covered.push_back(b);
            if (block_has_kill(info.value.id, b)) continue;
            for_each_successor(b, [&](BlockId succ) {
                if (succ.is_valid()) pending.push_back(succ.id);
            });
            for (u32 h : block_to_handlers[b]) {
                auto memo_it = handler_memo.find(h);
                bool cover;
                if (memo_it != handler_memo.end()) {
                    cover = memo_it->second;
                } else {
                    cover = handler_covered(h, info.value.id);
                    handler_memo[h] = cover;
                }
                if (cover) pending.push_back(ir_func->exception_handlers[h].handler_block.id);
            }
        }

        // Sort: block ids equal layout positions post-RPO, so sorted order is
        // layout order (needed for contiguous-run construction in build()).
        for (u32 i = 1; i < covered.size(); i++) {
            u32 v = covered[i];
            u32 j = i;
            while (j > 0 && covered[j - 1] > v) {
                covered[j] = covered[j - 1];
                j--;
            }
            covered[j] = v;
        }
    }
}

void BytecodeBuilder::compute_liveness(IRFunction* ir_func) {
    // Allocate live ranges for all SSA values in this function
    u32 num_values = ir_func->next_value_id;
    m_live_ranges.clear_keep_capacity();
    m_live_ranges.reserve(num_values);
    for (u32 i = 0; i < num_values; i++) {
        m_live_ranges.push_back(LiveRange{0, 0});
    }

    // Fused Pass 1+2+3: a single forward walk assigns definition points (Pass 1),
    // scans operands for last-uses (Pass 2), and extends each block param to its
    // predecessors' terminators (Pass 3). The three are independent (def_point vs
    // last_use_point), order-tolerant (mark_use is a max), and share one
    // program-point numbering, so one walk replaces three. Pass 4 (back-edge
    // extension) stays separate — it reads finalized last_use ranges.
    //
    // Correctness of folding Pass 3 into the forward walk: extending a *forward*
    // target's params is always a no-op (the param's def_point exceeds this
    // terminator point), and the transient mark is overwritten when the walk
    // later defines those params; a *back-edge* target was already defined
    // earlier in the walk, so its extension lands. Same result as three passes.
    u32 point = 0;
    for (IRBlock* block : ir_func->blocks) {
        // Block params: definition points (Pass 1).
        for (const auto& param : block->params) {
            if (param.value.is_valid() && param.value.id < num_values) {
                m_live_ranges[param.value.id].def_point = point;
                m_live_ranges[param.value.id].last_use_point = point;  // at least live at def
            }
            point++;
        }

        for (IRInst* inst : block->instructions) {
            // Result definition point (Pass 1).
            if (inst->result.is_valid() && inst->result.id < num_values) {
                m_live_ranges[inst->result.id].def_point = point;
                m_live_ranges[inst->result.id].last_use_point = point;  // at least live at def
            }
            // Operand last-uses (Pass 2), via the shared operand walker
            // (ir_optimize.hpp) — one op-shape enumeration for the whole
            // compiler instead of a per-pass copy.
            for_each_operand(inst, [&](ValueId& operand) {
                mark_use(m_live_ranges, operand, point);
            });
            point++;
        }

        // Terminator operand last-uses (Pass 2), plus extension of each jump
        // target's params to this terminator point (Pass 3). The extension keeps
        // the register allocator from reusing a block-param register before all
        // of that block's block-arg MOVs are emitted (parallel-assignment safety).
        u32 terminator_point = point;
        Terminator& term = block->terminator;
        for_each_terminator_operand(term, [&](ValueId& operand) {
            mark_use(m_live_ranges, operand, terminator_point);
        });
        auto extend_target_params = [&](const JumpTarget& target) {
            if (target.args.size() == 0) return;
            if (!target.block.is_valid() || target.block.id >= ir_func->blocks.size()) return;
            IRBlock* target_block = ir_func->blocks[target.block.id];
            for (u32 i = 0; i < target_block->params.size(); i++) {
                mark_use(m_live_ranges, target_block->params[i].value, terminator_point);
            }
        };
        switch (term.kind) {
            case TerminatorKind::Goto:
                extend_target_params(term.goto_target);
                break;
            case TerminatorKind::Branch:
                extend_target_params(term.branch.then_target);
                extend_target_params(term.branch.else_target);
                break;
            default:
                break;
        }
        point++;  // terminator slot
    }

    // Pass 4: extend live ranges for loop back edges
    // When a back edge jumps from block B to earlier block H, any value defined
    // BEFORE the loop (def_point < loop_start) but used INSIDE the loop must
    // stay live for the entire loop, since the register would be read again
    // when the loop iterates.
    // Build block info: first program point and terminator point for each block
    // (indexed by block index, which equals RPO position since blocks are in RPO order)
    struct BlockPointInfo {
        u32 first_point;
        u32 term_point;
    };
    Vector<BlockPointInfo> block_points;
    {
        u32 bp = 0;
        for (IRBlock* blk : ir_func->blocks) {
            u32 first = bp;
            bp += blk->params.size();
            bp += blk->instructions.size();
            u32 term = bp;
            bp++;
            block_points.push_back({first, term});
        }
    }

    // Process back edges in ascending loop_end order — a single pass that reaches
    // the same fixed point the former while(changed) iteration did, without
    // re-walking the block list each round. Rationale: extending a value only ever
    // *increases* its last_use_point, and each back edge processed later has a
    // loop_end >= the current one, so no already-processed loop can become
    // applicable again (its `last_use < loop_end` guard can only have been made
    // false by an extension, never re-satisfied). Block terminator points strictly
    // increase with block index, so walking blocks in order already visits back
    // edges in ascending loop_end order — no explicit sort needed. Nested loops
    // still compose: an inner loop's extension raises last_use into an enclosing
    // loop's range, and the enclosing loop (larger loop_end, processed later)
    // then extends it further.
    for (u32 bi = 0; bi < ir_func->blocks.size(); bi++) {
        IRBlock* blk = ir_func->blocks[bi];
        u32 loop_end = block_points[bi].term_point;

        auto extend_for_back_edge = [&](BlockId target_id) {
            if (!target_id.is_valid() || target_id.id >= ir_func->blocks.size()) return;
            if (target_id.id >= bi) return;  // Not a back edge

            // Back edge from bi to target_idx: extend values defined before the
            // loop but last-used inside it out to the loop's end.
            u32 loop_start = block_points[target_id.id].first_point;
            for (u32 vi = 0; vi < num_values; vi++) {
                auto& lr = m_live_ranges[vi];
                if (lr.def_point < loop_start &&
                    lr.last_use_point >= loop_start &&
                    lr.last_use_point < loop_end) {
                    lr.last_use_point = loop_end;
                }
            }
        };

        const Terminator& term = blk->terminator;
        switch (term.kind) {
            case TerminatorKind::Goto:
                extend_for_back_edge(term.goto_target.block);
                break;
            case TerminatorKind::Branch:
                extend_for_back_edge(term.branch.then_target.block);
                extend_for_back_edge(term.branch.else_target.block);
                break;
            default:
                break;
        }
    }

    // Pass 5: extend liveness of values tracked by cleanup records.
    // When a throw terminates a block, normal-path cleanup is skipped (the block
    // is unreachable), so the value may have no use after its last access. But
    // the VM's exception handler reads the register to perform cleanup, so the
    // register must hold the value at the throw site. Extend each cleanup
    // value's liveness to the end of its cleanup scope.
    //   - Owned locals: scope ends at end_block (matches the record's scope).
    //   - RefDec borrows (ref params): live for the WHOLE function — their
    //     record spans [0, code.size()), so pin the register to the final point
    //     so it holds the borrow at every possible throw site. Using end_block
    //     here would under-extend (functions whose paths all return/throw have
    //     no end block at the max PC), leaving the register reused past it and
    //     the unwind RefDec reading garbage.
    for (const auto& ci : ir_func->cleanup_info) {
        if (!ci.value.is_valid() || ci.value.id >= num_values) continue;
        u32 scope_end_point;
        if (ci.whole_function_scope) {
            // Ref params: pinned to the final point so the register holds the
            // borrow at every throw site (the record spans the whole body).
            if (block_points.empty()) continue;
            scope_end_point = block_points.back().term_point;
        } else {
            // Owned locals and ref locals: live to their block-derived scope end.
            if (!ci.end_block.is_valid() || ci.end_block.id >= block_points.size()) continue;
            scope_end_point = block_points[ci.end_block.id].term_point;
        }
        auto& lr = m_live_ranges[ci.value.id];
        if (scope_end_point > lr.last_use_point) {
            lr.last_use_point = scope_end_point;
        }
    }

    // Pass 5b: extend each tracked value across its whole unwind coverage
    // (compute_cleanup_coverage). The end_block-based extension above stops at
    // the scope's normal exit, but RPO lays a throw-terminated branch out
    // *after* that block; the extension records emitted for such blocks read
    // the value's register at the throw, so it must not be recycled there —
    // SSA liveness alone considers the value dead in a block with no use.
    for (u32 ci_idx = 0; ci_idx < ir_func->cleanup_info.size() &&
                         ci_idx < m_cleanup_covered_blocks.size(); ci_idx++) {
        const Vector<u32>& covered = m_cleanup_covered_blocks[ci_idx];
        if (covered.empty()) continue;
        const auto& info = ir_func->cleanup_info[ci_idx];
        if (!info.value.is_valid() || info.value.id >= num_values) continue;
        u32 max_block = covered.back();  // sorted, so back() is the layout-last
        if (max_block >= block_points.size()) continue;
        u32 end_point = block_points[max_block].term_point;
        auto& lr = m_live_ranges[info.value.id];
        if (end_point > lr.last_use_point) {
            lr.last_use_point = end_point;
        }
    }

    // Compute same-block flags: a value is same-block if its def_point and
    // last_use_point fall within the same block's range.
    // Cross-block values (used in a different block than defined) must NOT
    // have their registers reused from the free list, because the IR may have
    // partially-defined values (e.g., AND/OR short-circuit patterns where a
    // value is only defined on one branch). Fresh registers are zero-initialized
    // by the VM, preserving correct behavior for such patterns.
    m_value_same_block.clear_keep_capacity();
    m_value_same_block.reserve(num_values);
    for (u32 vi = 0; vi < num_values; vi++) {
        m_value_same_block.push_back(false);
    }
    // A value is same-block iff its def_point and last_use_point fall in one
    // block. Blocks partition the point space into contiguous ranges ordered by
    // first_point (block index == RPO position), and last_use_point >= def_point,
    // so this reduces to: does last_use_point lie within the range of the block
    // that contains def_point? Binary-search that block per value — O(values *
    // log blocks) instead of the former O(blocks * values) scan, which was the
    // dominant cost of compute_liveness on functions with many blocks.
    for (u32 vi = 0; vi < num_values; vi++) {
        const auto& lr = m_live_ranges[vi];
        // Last block whose first_point <= def_point == the block containing it
        // (ranges are contiguous, so the next block starts past def_point).
        u32 lo = 0, hi = block_points.size();
        while (lo < hi) {
            u32 mid = lo + (hi - lo) / 2;
            if (block_points[mid].first_point <= lr.def_point) lo = mid + 1;
            else hi = mid;
        }
        if (lo != 0 && lr.last_use_point <= block_points[lo - 1].term_point) {
            m_value_same_block[vi] = true;
        }
    }

    // Force block params cross-block.
    // Block params receive values from predecessor blocks, so they must not
    // reuse freed registers (need fresh zero-initialized regs).
    // With RPO ordering, their liveness is now correct, so they CAN be freed
    // after their last use (unlike before where they were permanently pinned).
    for (IRBlock* block : ir_func->blocks) {
        for (const auto& param : block->params) {
            if (param.value.is_valid() && param.value.id < num_values) {
                m_value_same_block[param.value.id] = false;
            }
        }
    }
}

void BytecodeBuilder::expire_before(u32 current_point) {
    // Return every register whose value dies before current_point to the free
    // set. m_active is sorted by last_use ascending, so the expiring entries are
    // a prefix — count it once, free those registers, then shift the surviving
    // tail down a single time. (The old code shifted the whole tail per expired
    // entry: O(prefix * active). Now O(active). See §4.2.)
    u32 expired = 0;
    while (expired < m_active.size() && m_active[expired].last_use < current_point) {
        u8 freed_reg = m_active[expired].reg;
        // Don't return scratch registers to the free set — they're permanently reserved
        if (!m_has_spilling || (freed_reg != m_scratch_regs[0] && freed_reg != m_scratch_regs[1])) {
            free_reg_add(freed_reg);
        }
        expired++;
    }
    if (expired > 0) {
        for (u32 i = expired; i < m_active.size(); i++) {
            m_active[i - expired] = m_active[i];
        }
        for (u32 i = 0; i < expired; i++) m_active.pop_back();
    }
}

u16 BytecodeBuilder::add_constant(const BCConstant& c) {
    size_t index = m_current_func->constants.size();
    if (index > 0xFFFF) {
        // The constant-pool index is a u16; wrapping it would silently make
        // LOAD_CONST read the wrong constant. Reject instead.
        report_error("Constant pool overflow: function has more than 65535 constants");
        return 0;
    }
    m_current_func->constants.push_back(c);
    return static_cast<u16>(index);
}

// RK helpers: dedup against the existing pool so a single constant value reused
// across many sites costs one pool slot, not N. Linear scan is fine — pools are
// typically <100 entries per function.
i32 BytecodeBuilder::get_or_add_int_constant(i64 value) {
    const auto& constants = m_current_func->constants;
    for (u32 i = 0; i < constants.size(); i++) {
        if (constants[i].type == BCConstant::Int && constants[i].as_int == value) {
            return static_cast<i32>(i);
        }
    }
    return static_cast<i32>(add_constant(BCConstant::make_int(value)));
}

i32 BytecodeBuilder::get_or_add_float_constant(f64 value) {
    const auto& constants = m_current_func->constants;
    // Bitwise compare so -0.0 and +0.0 stay distinct (matches IEEE 754 identity,
    // not equality — important for roundtripping).
    u64 bits;
    memcpy(&bits, &value, sizeof(bits));
    for (u32 i = 0; i < constants.size(); i++) {
        if (constants[i].type == BCConstant::Float) {
            u64 existing_bits;
            memcpy(&existing_bits, &constants[i].as_float, sizeof(existing_bits));
            if (existing_bits == bits) {
                return static_cast<i32>(i);
            }
        }
    }
    return static_cast<i32>(add_constant(BCConstant::make_float(value)));
}

Opcode BytecodeBuilder::rk_opcode_for(IROp op) const {
    switch (op) {
        case IROp::AddI: return Opcode::ADD_I_RK;
        case IROp::SubI: return Opcode::SUB_I_RK;
        case IROp::MulI: return Opcode::MUL_I_RK;
        case IROp::AddF: return Opcode::ADD_F_RK;
        case IROp::SubF: return Opcode::SUB_F_RK;
        case IROp::MulF: return Opcode::MUL_F_RK;
        case IROp::AddD: return Opcode::ADD_D_RK;
        case IROp::SubD: return Opcode::SUB_D_RK;
        case IROp::MulD: return Opcode::MUL_D_RK;
        case IROp::DivD: return Opcode::DIV_D_RK;
        // Integer comparisons intentionally absent: fuse_compare_branch() turns
        // `LT_I + JMP_IF_NOT` into a single `JMP_IF_GE_I`, which is faster than
        // `LT_I_RK + JMP_IF_NOT` (one dispatch vs two). Integer compares with
        // constant RHS will become RK once `JMP_IF_*_I_RK` fused variants land.
        // The Opcode::*_I_RK entries exist in bytecode.hpp for that future work.
        case IROp::EqD:  return Opcode::EQ_D_RK;
        case IROp::NeD:  return Opcode::NE_D_RK;
        case IROp::LtD:  return Opcode::LT_D_RK;
        case IROp::LeD:  return Opcode::LE_D_RK;
        case IROp::GtD:  return Opcode::GT_D_RK;
        case IROp::GeD:  return Opcode::GE_D_RK;
        default:         return Opcode::NOP;
    }
}

bool BytecodeBuilder::is_commutative_binary(IROp op) {
    switch (op) {
        case IROp::AddI: case IROp::MulI:
        case IROp::AddF: case IROp::MulF:
        case IROp::AddD: case IROp::MulD:
        case IROp::EqI: case IROp::NeI:
        case IROp::EqD: case IROp::NeD:
        case IROp::EqF: case IROp::NeF:
        case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor:
        case IROp::And: case IROp::Or:
            return true;
        default:
            return false;
    }
}

bool BytecodeBuilder::try_emit_rk_binary(IRInst* inst, u8 dst) {
    Opcode rk_op = rk_opcode_for(inst->op);
    if (rk_op == Opcode::NOP) return false;

    ValueId left_id = inst->binary.left;
    ValueId right_id = inst->binary.right;

    auto def_of = [this](ValueId v) -> IRInst* {
        if (!v.is_valid() || v.id >= m_current_ir_func->values_by_id.size()) return nullptr;
        return m_current_ir_func->values_by_id[v.id];
    };

    auto is_rk_eligible_const = [](IRInst* def) -> bool {
        if (!def) return false;
        return def->op == IROp::ConstInt || def->op == IROp::ConstF || def->op == IROp::ConstD;
    };

    IRInst* right_def = def_of(right_id);
    IRInst* left_def = def_of(left_id);
    bool right_const = is_rk_eligible_const(right_def);
    bool left_const = is_rk_eligible_const(left_def);

    // Canonicalize: for commutative ops with constant only on LHS, swap so the
    // constant lands on RHS where RK reads it.
    if (is_commutative_binary(inst->op) && left_const && !right_const) {
        std::swap(left_id, right_id);
        std::swap(left_def, right_def);
        std::swap(left_const, right_const);
    }

    if (!right_const) return false;

    // Resolve the constant to a pool index — either reuse or add.
    i32 pool_idx = -1;
    switch (right_def->op) {
        case IROp::ConstInt:
            pool_idx = get_or_add_int_constant(right_def->const_data.int_val);
            break;
        case IROp::ConstF: {
            // f32 constants are stored as Int (raw bit pattern in low 32 bits)
            // — the OP(*_F_RK) handlers reload via reg_as_f32, which reads the
            // low 32 bits as f32 bit pattern.
            f32 fval = right_def->const_data.f32_val;
            u32 bits;
            memcpy(&bits, &fval, sizeof(bits));
            pool_idx = get_or_add_int_constant(static_cast<i64>(static_cast<u64>(bits)));
            break;
        }
        case IROp::ConstD:
            pool_idx = get_or_add_float_constant(right_def->const_data.f64_val);
            break;
        default:
            return false;
    }

    // RK encoding has 8 bits for the constant index; pools larger than 256
    // entries fall back to materialization. add_constant returns u16 so the
    // pool can grow well past 256 — this guard is essential.
    if (pool_idx < 0 || pool_idx > 0xFF) return false;

    u8 left_reg = ensure_in_register(left_id, 0);
    u32 cmp_pc = m_current_func->code.size();
    emit_abc(rk_op, dst, left_reg, static_cast<u8>(pool_idx));
    mark_unfusable_if_cross_block(inst, cmp_pc);
    return true;
}

void BytecodeBuilder::mark_unfusable_if_cross_block(IRInst* inst, u32 cmp_pc) {
    if (!is_comparison_op(inst->op)) return;
    if (!inst->result.is_valid() || inst->result.id >= m_value_same_block.size()) return;
    if (!m_value_same_block[inst->result.id]) {
        m_unfusable_cmp_pcs.insert(cmp_pc);
    }
}

void BytecodeBuilder::emit(u32 instr) {
    m_current_func->code.push_back(instr);
}

void BytecodeBuilder::emit_abc(Opcode op, u8 a, u8 b, u8 c) {
    emit(encode_abc(op, a, b, c));
}

void BytecodeBuilder::emit_abi(Opcode op, u8 a, u16 imm) {
    emit(encode_abi(op, a, imm));
}

void BytecodeBuilder::emit_aoff(Opcode op, u8 a, i16 offset) {
    emit(encode_aoff(op, a, offset));
}

void BytecodeBuilder::emit_call_args(Span<ValueId> args, IRFunction* callee_func,
                                     u8 first_arg_reg, bool structs_in_registers) {
    u8 arg_reg_offset = 0;
    for (u32 i = 0; i < args.size(); i++) {
        ValueId arg_val = args[i];
        u8 arg_src = ensure_in_register(arg_val, 0);
        u8 arg_dst = static_cast<u8>(first_arg_reg + arg_reg_offset);

        // out/inout parameters receive the pointer already computed by
        // gen_lvalue_addr — pass it directly.
        bool param_is_ptr = (callee_func && i < callee_func->param_is_ptr.size() &&
                             callee_func->param_is_ptr[i]);
        if (param_is_ptr) {
            if (arg_src != arg_dst) {
                emit_abc(Opcode::MOV, arg_dst, arg_src, 0);
            }
            arg_reg_offset += 1;
            continue;
        }

        Type* arg_type = value_type_of(arg_val.id);
        if (arg_type && arg_type->kind == TypeKind::Weak) {
            // Weak ref: {pointer, generation} in two consecutive registers.
            if (arg_src != arg_dst) {
                emit_abc(Opcode::MOV, arg_dst, arg_src, 0);
                emit_abc(Opcode::MOV, static_cast<u8>(arg_dst + 1), static_cast<u8>(arg_src + 1), 0);
            }
            arg_reg_offset += 2;
            continue;
        }

        u32 arg_slot_count = structs_in_registers ? get_struct_slot_count(arg_type) : 0;
        if (arg_slot_count > 0 && arg_slot_count <= 4) {
            // Small struct: load struct data from memory to consecutive registers
            emit_abc(Opcode::STRUCT_LOAD_REGS, arg_dst, arg_src, static_cast<u8>(arg_slot_count));
            emit(0);  // Padding word
            arg_reg_offset += static_cast<u8>((arg_slot_count + 1) / 2);
        } else {
            // Large structs pass their pointer; everything else its value.
            if (arg_src != arg_dst) {
                emit_abc(Opcode::MOV, arg_dst, arg_src, 0);
            }
            arg_reg_offset += 1;
        }
    }
}

void BytecodeBuilder::emit_small_struct_return_unpack(u8 dst, u32 ret_slot_count) {
    if (ret_slot_count == 0 || ret_slot_count > 4) return;
    u32 stack_offset = m_next_stack_slot;
    m_next_stack_slot += ret_slot_count;

    // Scratch register for the stack address: the first window slot above the
    // packed return slots. It is inside the call's reserved window —
    // reserve_call_window sized it with extra_regs_for_return >= 1 exactly
    // when the return is a small struct — and window space above the packed
    // slots is transient argument space, dead once the call has returned; any
    // SSA value sharing the register is defined only after this point (the
    // same ordering argument reserve_call_window itself relies on). The
    // previous bump_register() here permanently reserved one register PER
    // CALL SITE, so a few hundred small-struct-returning calls overflowed the
    // 255-register frame despite low real pressure.
    u8 temp_reg = static_cast<u8>(dst + (ret_slot_count + 1) / 2);
    emit_abi(Opcode::STACK_ADDR, temp_reg, static_cast<u16>(stack_offset));
    emit_abc(Opcode::STRUCT_STORE_REGS, temp_reg, dst, static_cast<u8>(ret_slot_count));
    emit(0);  // Padding word
    // dst now points to the stack copy of the returned struct.
    emit_abc(Opcode::MOV, dst, temp_reg, 0);
}

void BytecodeBuilder::lower_direct_call(IRInst* inst, u8 dst, StringView func_name,
                                        Span<ValueId> args, const char* not_found_error) {
    auto it = m_func_indices.find(func_name);
    if (it == m_func_indices.end()) {
        report_error(not_found_error);
        return;
    }
    u32 func_idx = it->second;
    IRFunction* callee_func = m_ir_module->functions[func_idx];

    // Calling convention: arguments follow the destination/return registers.
    u8 ret_reg_count = static_cast<u8>(get_value_reg_count(inst->type));
    emit_call_args(args, callee_func, static_cast<u8>(dst + ret_reg_count), true);

    // Two-word CALL: word 1 = [CALL][dst][_][arg_count], word 2 = [func_idx:32]
    emit_abc(Opcode::CALL, dst, 0, static_cast<u8>(args.size()));
    emit(func_idx);
    for (u32 i = 0; i < args.size(); i++) note_call_use(args[i]);

    emit_small_struct_return_unpack(dst, get_struct_slot_count(inst->type));
}

void BytecodeBuilder::lower_instruction(IRInst* inst) {
    // Skip-load constants have no register and no LOAD; bail before
    // get_result_register, which would error on the missing allocation.
    if (is_skip_load_const(inst)) {
        return;
    }
    u8 dst = get_result_register(inst->result);

    switch (inst->op) {
        case IROp::ConstNull:
            emit_abi(Opcode::LOAD_NULL, dst, 0);
            spill_if_needed(inst->result, dst);
            break;

        case IROp::ConstBool:
            if (inst->const_data.bool_val) {
                emit_abi(Opcode::LOAD_TRUE, dst, 0);
            } else {
                emit_abi(Opcode::LOAD_FALSE, dst, 0);
            }
            spill_if_needed(inst->result, dst);
            break;

        case IROp::ConstInt:
        case IROp::ConstF:
        case IROp::ConstD:
            // Skip-load case is handled at the top of lower_instruction.
            emit_const_load(inst, dst);
            spill_if_needed(inst->result, dst);
            break;

        case IROp::ConstString: {
            StringView sv = inst->const_data.string_val;
            u16 const_idx = add_constant(BCConstant::make_string(sv.data(), sv.size()));
            emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            spill_if_needed(inst->result, dst);
            break;
        }

        // Binary operations
        case IROp::AddI:
        case IROp::SubI:
        case IROp::MulI:
        case IROp::DivI:
        case IROp::ModI:
        case IROp::DivU:
        case IROp::ModU:
        case IROp::AddF:
        case IROp::SubF:
        case IROp::MulF:
        case IROp::DivF:
        case IROp::AddD:
        case IROp::SubD:
        case IROp::MulD:
        case IROp::DivD:
        case IROp::BitAnd:
        case IROp::BitOr:
        case IROp::BitXor:
        case IROp::Shl:
        case IROp::Shr:
        case IROp::UShr:
        case IROp::EqI:
        case IROp::NeI:
        case IROp::LtI:
        case IROp::LeI:
        case IROp::GtI:
        case IROp::GeI:
        case IROp::LtU:
        case IROp::LeU:
        case IROp::GtU:
        case IROp::GeU:
        case IROp::EqF:
        case IROp::NeF:
        case IROp::LtF:
        case IROp::LeF:
        case IROp::GtF:
        case IROp::GeF:
        case IROp::EqD:
        case IROp::NeD:
        case IROp::LtD:
        case IROp::LeD:
        case IROp::GtD:
        case IROp::GeD:
        case IROp::And:
        case IROp::Or: {
            // Try RK lowering first: if RHS is a compile-time constant (or, for
            // commutative ops, either side), emit the *_RK form which folds the
            // LOAD_INT/LOAD_CONST into the constant-pool index field. Saves one
            // dispatch + memory write per iteration in tight loops.
            if (try_emit_rk_binary(inst, dst)) {
                canonicalize_u32(inst, dst);
                spill_if_needed(inst->result, dst);
                break;
            }
            // Reload operands first (scratch[1] for right, then scratch[0] for left)
            // Order matters: if dst is spilled, it uses scratch[0], so load right into
            // scratch[1] first, then left into scratch[0] which becomes the dst.
            u8 right = ensure_in_register(inst->binary.right, 1);
            u8 left = ensure_in_register(inst->binary.left, 0);
            u32 cmp_pc = m_current_func->code.size();
            emit_abc(get_opcode(inst->op), dst, left, right);
            // Mark compares as unfusable when their SSA result is live past
            // this block's terminator — otherwise fuse_compare_branch would drop the
            // register write that the later block's read depends on.
            mark_unfusable_if_cross_block(inst, cmp_pc);
            canonicalize_u32(inst, dst);
            spill_if_needed(inst->result, dst);
            break;
        }

        // Unary operations
        case IROp::NegI:
        case IROp::NegF:
        case IROp::NegD:
        case IROp::BitNot:
        case IROp::Not:
        case IROp::I_TO_F64:
        case IROp::F64_TO_I:
        case IROp::I_TO_B:
        case IROp::B_TO_I: {
            u8 src = ensure_in_register(inst->unary, 1);
            emit_abc(get_opcode(inst->op), dst, src, 0);
            canonicalize_u32(inst, dst);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::Copy: {
            u8 src = ensure_in_register(inst->unary, 1);
            if (dst != src) {
                emit_abc(Opcode::MOV, dst, src, 0);
            }
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::BlockArg:
            // Block arguments are handled by MOV instructions at jump sites
            // The value should already be in the register (or spill slot)
            break;

        case IROp::Call:
            lower_direct_call(inst, dst, inst->call.func_name, inst->call.args,
                              "Internal error: function not found during bytecode lowering");
            break;

        case IROp::CallNative: {
            // Similar to Call but uses CALL_NATIVE opcode, with args at dst+1.
            // Natives take structs by pointer, so no STRUCT_LOAD_REGS packing
            // (structs_in_registers = false); a `weak T` still occupies two
            // consecutive registers — the same convention as CALL, and what
            // compute_call_arg_reg_count already sized this window for.
            emit_call_args(inst->call.args, nullptr, static_cast<u8>(dst + 1), false);

            // Two-word CALL_NATIVE: word 1 = [op][dst][_][arg_count], word 2 = [func_idx:32]
            emit_abc(Opcode::CALL_NATIVE, dst, 0, static_cast<u8>(inst->call.args.size()));
            emit(inst->call.native_index);
            for (u32 i = 0; i < inst->call.args.size(); i++) note_call_use(inst->call.args[i]);
            break;
        }

        case IROp::CallExternal:
            // Cross-module function call — resolved at link time via static
            // linking. After linking, all functions are in the same module, so
            // this lowers to a regular CALL.
            lower_direct_call(inst, dst, inst->call_external.func_name, inst->call_external.args,
                              "Internal error: external function not found during linking");
            break;

        case IROp::CallIndirect: {
            // Indirect call through a closure value. The interpreter reads the
            // call function index from the env's first u32 field and dispatches
            // with the env pointer as the first (hidden) argument; explicit args
            // follow at consecutive registers starting from first_arg_reg.
            u8 closure_reg = ensure_in_register(inst->call_indirect.callee, 0);
            u8 ret_reg_count = static_cast<u8>(get_value_reg_count(inst->type));
            emit_call_args(inst->call_indirect.args, nullptr,
                           static_cast<u8>(dst + ret_reg_count), true);

            // Two-word CALL_INDIRECT: word 1 = [op][dst][closure_reg][arg_count],
            // word 2 = reserved (future inline-cache slot).
            emit_abc(Opcode::CALL_INDIRECT, dst, closure_reg,
                     static_cast<u8>(inst->call_indirect.args.size()));
            emit(0u);
            for (u32 i = 0; i < inst->call_indirect.args.size(); i++) {
                note_call_use(inst->call_indirect.args[i]);
            }
            note_call_use(inst->call_indirect.callee);

            emit_small_struct_return_unpack(dst, get_struct_slot_count(inst->type));
            break;
        }

        case IROp::IndexGet: {
            u8 obj_reg = ensure_in_register(inst->index_data.container, 0);
            u8 idx_reg = ensure_in_register(inst->index_data.index, 0);
            Opcode op = (inst->index_data.kind == ContainerKind::List)
                ? Opcode::INDEX_GET_LIST : Opcode::INDEX_GET_MAP;
            emit_abc(op, dst, obj_reg, idx_reg);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::IndexAddr: {
            // Element address (out/inout lvalue): bounds-/key-checked pointer into
            // the container's backing buffer, stored in dst as a raw pointer.
            u8 obj_reg = ensure_in_register(inst->index_data.container, 0);
            u8 idx_reg = ensure_in_register(inst->index_data.index, 0);
            Opcode op = (inst->index_data.kind == ContainerKind::List)
                ? Opcode::INDEX_ADDR_LIST : Opcode::INDEX_ADDR_MAP;
            emit_abc(op, dst, obj_reg, idx_reg);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::IndexTryAddr: {
            // Nullable map value-slot address (0 on miss, no trap) — Map only.
            u8 obj_reg = ensure_in_register(inst->index_data.container, 0);
            u8 idx_reg = ensure_in_register(inst->index_data.index, 0);
            emit_abc(Opcode::INDEX_TRYADDR_MAP, dst, obj_reg, idx_reg);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::IndexSet: {
            u8 obj_reg = ensure_in_register(inst->index_data.container, 0);
            u8 idx_reg = ensure_in_register(inst->index_data.index, 0);
            u8 val_reg = ensure_in_register(inst->index_data.value, 0);
            Opcode op = (inst->index_data.kind == ContainerKind::List)
                ? Opcode::INDEX_SET_LIST : Opcode::INDEX_SET_MAP;
            emit_abc(op, obj_reg, idx_reg, val_reg);
            break;
        }

        case IROp::GetField: {
            // Format: [GET_FIELD dst obj slot_count] + [slot_offset:16 padding:16]
            u8 obj = ensure_in_register(inst->field.object, 1);
            u8 slot_count = static_cast<u8>(inst->field.slot_count);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::GET_FIELD, dst, obj, slot_count);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            // A u32 field ≥ 2^31 sign-extends through GET_FIELD's 1-slot load;
            // re-zero-extend so the canonical-u32 invariant holds.
            canonicalize_u32(inst, dst);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::GetFieldAddr: {
            // Format: [GET_FIELD_ADDR dst obj 0] + [slot_offset:16 padding:16]
            // Computes: dst = obj_ptr + slot_offset * 4 (pointer arithmetic)
            u8 obj = ensure_in_register(inst->field.object, 1);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::GET_FIELD_ADDR, dst, obj, 0);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::SetField: {
            // Format: [SET_FIELD obj val slot_count] + [slot_offset:16 padding:16]
            u8 obj = ensure_in_register(inst->field.object, 0);
            u8 val = ensure_in_register(inst->store_value, 1);
            u8 slot_count = static_cast<u8>(inst->field.slot_count);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::SET_FIELD, obj, val, slot_count);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            break;
        }

        case IROp::RefInc: {
            // Record this RefInc's PC so a call-site receiver borrow's cleanup
            // record can start exactly here (m_ref_inc_pcs). Harmless for other
            // RefIncs — only call_borrow records read it, and a borrow value has
            // exactly one RefInc.
            m_ref_inc_pcs[inst->unary.id] = static_cast<u32>(m_current_func->code.size());
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::REF_INC, ptr, 0, 0);
            break;
        }

        case IROp::RefDec: {
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::REF_DEC, ptr, 0, 0);
            record_cleanup_kill_pc(inst->unary);
            break;
        }

        case IROp::StrRetain: {
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::STR_RETAIN, ptr, 0, 0);
            break;
        }

        case IROp::StrRelease: {
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::STR_RELEASE, ptr, 0, 0);
            record_cleanup_kill_pc(inst->unary);
            break;
        }

        case IROp::ContainerPin: {
            // Like RefInc, record the PC so the Unpin cleanup record can start
            // exactly here (the pinned-copy value has exactly one ContainerPin).
            m_ref_inc_pcs[inst->unary.id] = static_cast<u32>(m_current_func->code.size());
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::CONTAINER_PIN, ptr, 0, 0);
            break;
        }

        case IROp::ContainerUnpin: {
            u8 ptr = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::CONTAINER_UNPIN, ptr, 0, 0);
            break;
        }

        case IROp::WeakCheck: {
            u8 weak = ensure_in_register(inst->unary, 1);
            emit_abc(Opcode::WEAK_CHECK, dst, weak, 0);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::WeakCreate: {
            u8 src = ensure_in_register(inst->unary, 1);
            emit_abc(Opcode::WEAK_CREATE, dst, src, 0);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::New: {
            StringView type_name = inst->new_data.type_name;

            // Lookup or register type in module's type table
            auto it = m_type_indices.find(type_name);
            u16 type_idx;
            if (it != m_type_indices.end()) {
                type_idx = it->second;
            } else {
                Type* uniq_type = inst->type;
                Type* struct_type = uniq_type->base_type();
                u32 size_bytes = struct_type->struct_info.slot_count * sizeof(u32);

                type_idx = static_cast<u16>(m_module->types.size());
                BCTypeInfo info{type_name, size_bytes, struct_type->struct_info.slot_count};
                // Record the struct's destructor so a type-erased delete
                // (BCDeleteDesc::Closure — used to drop an erased Coro<T> whose
                // concrete state struct isn't statically known) can dispatch it
                // by runtime type_id. Mirrors the closure-env registration below.
                if (struct_has_default_dtor(struct_type)) {
                    u16 dtor_idx = lookup_destructor_index(struct_type);
                    if (dtor_idx != 0) info.dtor_func_idx = dtor_idx;
                }
                m_module->types.push_back(info);
                m_type_indices[type_name] = type_idx;
            }

            emit_abi(Opcode::NEW_OBJ, dst, type_idx);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::AssertHeap: {
            u8 src = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::ASSERT_HEAP, src, 0, 0);
            break;
        }

        case IROp::Closure: {
            // Allocate the env struct, store __call_idx in its first u32 field,
            // then store any captured values in subsequent fields.
            StringView env_name = inst->closure.env_struct_name;
            Type* env_type = m_type_env->named_type_by_name(env_name);
            if (!env_type || !env_type->is_struct()) {
                report_error("Internal error: closure env struct type not registered");
                break;
            }

            // Register the env type in the module's type table if needed.
            auto type_it = m_type_indices.find(env_name);
            u16 type_idx;
            if (type_it != m_type_indices.end()) {
                type_idx = type_it->second;
            } else {
                u32 size_bytes = env_type->struct_info.slot_count * sizeof(u32);
                type_idx = static_cast<u16>(m_module->types.size());
                BCTypeInfo env_info{env_name, size_bytes, env_type->struct_info.slot_count};
                // Record the env's synthesized destructor (built in the IR pass)
                // so the closure delete can dispatch it by type_id. 0 from
                // lookup means "no destructor"; map it to the 0xFFFFFFFF sentinel.
                if (struct_has_default_dtor(env_type)) {
                    u16 dtor_idx = lookup_destructor_index(env_type);
                    if (dtor_idx != 0) env_info.dtor_func_idx = dtor_idx;
                }
                m_module->types.push_back(env_info);
                m_type_indices[env_name] = type_idx;
            }

            // Resolve the call function's index (added to m_func_indices when its
            // IRFunction was lowered earlier in the pass).
            auto func_it = m_func_indices.find(inst->closure.call_function_name);
            if (func_it == m_func_indices.end()) {
                report_error("Internal error: closure call function not found during lowering");
                break;
            }
            u32 call_idx_value = func_it->second;

            // NEW_OBJ — produces env pointer in `dst`.
            emit_abi(Opcode::NEW_OBJ, dst, type_idx);

            // Load call_idx as a constant (LOAD_INT if it fits, else LOAD_CONST).
            u8 idx_reg = bump_register();
            if (call_idx_value <= 0x7FFF) {
                emit_abi(Opcode::LOAD_INT, idx_reg, static_cast<u16>(call_idx_value));
            } else {
                u16 const_idx = add_constant(BCConstant::make_int(static_cast<i64>(call_idx_value)));
                emit_abi(Opcode::LOAD_CONST, idx_reg, const_idx);
            }

            // SET_FIELD(env, call_idx, slot_count=1) at slot_offset 0.
            emit_abc(Opcode::SET_FIELD, dst, idx_reg, 1);
            emit(0u);  // slot_offset = 0

            // Store each capture into its corresponding field. The env struct's
            // fields[0] is __call_idx (already populated); captures live at fields[1..].
            // For value-struct fields, the source ValueId holds a pointer to the
            // struct's bytes, so we use a memory-to-memory STRUCT_COPY (computing
            // the dest via GET_FIELD_ADDR). For primitive / ref / weak fields,
            // the source is in registers and SET_FIELD packs it directly.
            for (u32 i = 0; i < inst->closure.captures.size(); i++) {
                const FieldInfo& field = env_type->struct_info.fields[1 + i];
                u8 cap_reg = ensure_in_register(inst->closure.captures[i], 0);

                if (field.type && field.type->is_struct()) {
                    // dest_addr = env_ptr + field.slot_offset
                    u8 dest_addr = bump_register();
                    emit_abc(Opcode::GET_FIELD_ADDR, dest_addr, dst, 0);
                    emit(static_cast<u32>(field.slot_offset));
                    // STRUCT_COPY (specialized for small slot counts)
                    Opcode op;
                    switch (field.slot_count) {
                        case 1: op = Opcode::STRUCT_COPY_1; break;
                        case 2: op = Opcode::STRUCT_COPY_2; break;
                        case 3: op = Opcode::STRUCT_COPY_3; break;
                        case 4: op = Opcode::STRUCT_COPY_4; break;
                        default: op = Opcode::STRUCT_COPY; break;
                    }
                    emit_abc(op, dest_addr, cap_reg,
                             op == Opcode::STRUCT_COPY ? static_cast<u8>(field.slot_count) : 0);
                } else {
                    emit_abc(Opcode::SET_FIELD, dst, cap_reg,
                             static_cast<u8>(field.slot_count));
                    emit(static_cast<u32>(field.slot_offset));
                }
            }

            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::FuncIndex: {
            // Materialize the named function's runtime index as a constant — the
            // same late-binding used for a closure's __call_idx (resolved from
            // m_func_indices, pre-populated for every module function above).
            auto func_it = m_func_indices.find(inst->func_index.func_name);
            if (func_it == m_func_indices.end()) {
                report_error("Internal error: FuncIndex target function not found during lowering");
                break;
            }
            u32 idx_value = func_it->second;
            if (idx_value <= 0x7FFF) {
                emit_abi(Opcode::LOAD_INT, dst, static_cast<u16>(idx_value));
            } else {
                u16 const_idx = add_constant(BCConstant::make_int(static_cast<i64>(idx_value)));
                emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            }
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::Delete: {
            u8 ptr_reg = ensure_in_register(inst->unary, 0);
            if (!inst->type || inst->type->kind == TypeKind::Void) {
                // Raw free (explicit delete after manual destructor call)
                emit_abc(Opcode::DEL_OBJ, ptr_reg, 0, 0);
            } else {
                // Typed delete — build descriptor tree from type
                u16 desc_idx = build_delete_desc(inst->type);
                emit_abi(Opcode::DELETE, ptr_reg, desc_idx);
            }
            record_cleanup_kill_pc(inst->unary);
            break;
        }

        case IROp::Nullify: {
            // Record the current PC as the point where ownership was transferred.
            // The cleanup record builder uses this to end the scope early.
            // No runtime instruction is emitted — Nullify is a compile-time annotation.
            m_nullify_pcs[inst->unary.id] = static_cast<u32>(m_current_func->code.size());
            // Anchored kill for coverage runs: ownership actually ended at the
            // last same-block ownership event — a release op (whose PC the
            // Delete/StrRelease/RefDec lowering already recorded) or a
            // consuming call (m_call_use_pcs). Instructions lowered between
            // that event and this annotation (return-value materialization,
            // SSA nulling) are not owned coverage: a throw escaping the
            // consuming callee surfaces at exactly the call boundary, when the
            // callee already owns — and on unwind frees — the value, so
            // covering the gap would free it twice.
            if (inst->unary.is_valid()) {
                auto kill_it = m_cleanup_kill_pcs.find(inst->unary.id);
                if (kill_it != m_cleanup_kill_pcs.end()) {
                    u32 anchor = static_cast<u32>(m_current_func->code.size());
                    u32 candidate = 0;
                    if (!kill_it->second.empty() && kill_it->second.back() >= m_block_start_pc) {
                        candidate = kill_it->second.back();
                    }
                    auto use_it = m_call_use_pcs.find(inst->unary.id);
                    if (use_it != m_call_use_pcs.end() && use_it->second > candidate) {
                        candidate = use_it->second;
                    }
                    if (candidate != 0 && candidate < anchor) anchor = candidate;
                    kill_it.value().push_back(anchor);
                }
            }
            break;
        }

        case IROp::StackAlloc: {
            // Allocate slots on the local stack
            u32 slot_count = inst->stack_alloc.slot_count;
            u32 slot_offset = m_next_stack_slot;
            m_next_stack_slot += slot_count;

            // Record the stack slot offset for this value

            // Emit STACK_ADDR to get a pointer to the allocated space
            emit_abi(Opcode::STACK_ADDR, dst, static_cast<u16>(slot_offset));
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::GlobalAddr: {
            // Pointer into the module's persistent global slot array.
            emit_abi(Opcode::GLOBAL_ADDR, dst, static_cast<u16>(inst->global_data.slot_offset));
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::StructCopy: {
            // Memory-to-memory struct copy. Pick the specialized opcode for
            // small slot counts (1-4 covers Vec2/Vec3/Color and similar);
            // STRUCT_COPY's runtime loop becomes straight-line stores.
            u8 dest_ptr = ensure_in_register(inst->struct_copy.dest_ptr, 0);
            u8 src_ptr = ensure_in_register(inst->struct_copy.source_ptr, 1);
            u32 slot_count = inst->struct_copy.slot_count;
            Opcode op;
            switch (slot_count) {
                case 1: op = Opcode::STRUCT_COPY_1; break;
                case 2: op = Opcode::STRUCT_COPY_2; break;
                case 3: op = Opcode::STRUCT_COPY_3; break;
                case 4: op = Opcode::STRUCT_COPY_4; break;
                default: op = Opcode::STRUCT_COPY; break;
            }
            emit_abc(op, dest_ptr, src_ptr, op == Opcode::STRUCT_COPY ? static_cast<u8>(slot_count) : 0);
            break;
        }

        case IROp::LoadPtr: {
            // Load value through pointer - reuse GET_FIELD with offset 0
            u8 ptr_reg = ensure_in_register(inst->load_ptr.ptr, 1);
            u8 slot_count = static_cast<u8>(inst->load_ptr.slot_count);
            emit_abc(Opcode::GET_FIELD, dst, ptr_reg, slot_count);
            emit(0);  // offset = 0
            // Same sign-extension hazard as GetField for a u32 load-through-pointer.
            canonicalize_u32(inst, dst);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::StorePtr: {
            // Store value through pointer - reuse SET_FIELD with offset 0
            u8 ptr_reg = ensure_in_register(inst->store_ptr.ptr, 0);
            u8 val_reg = ensure_in_register(inst->store_ptr.value, 1);
            u8 slot_count = static_cast<u8>(inst->store_ptr.slot_count);
            emit_abc(Opcode::SET_FIELD, ptr_reg, val_reg, slot_count);
            emit(0);  // offset = 0
            break;
        }

        case IROp::Cast: {
            u8 src = ensure_in_register(inst->cast.source, 1);
            Type* source_type = inst->cast.source_type;
            Type* target_type = inst->type;

            emit_cast_bytecode(dst, src, source_type, target_type);
            spill_if_needed(inst->result, dst);
            break;
        }

        case IROp::Throw: {
            u8 exc_reg = ensure_in_register(inst->unary, 0);
            emit_abc(Opcode::THROW, exc_reg, 0, 0);
            break;
        }

        case IROp::Yield: {
            // Should never reach bytecode lowering - coroutine lowering pass removes all Yields
            assert(false && "IROp::Yield should have been lowered by coroutine_lower()");
            break;
        }
    }
}

void BytecodeBuilder::emit_block_args(const JumpTarget& target) {
    if (!target.block.is_valid() || target.block.id >= m_current_ir_func->blocks.size()) return;
    IRBlock* target_block = m_current_ir_func->blocks[target.block.id];

    // A jump must pass exactly one argument per target block parameter —
    // silently truncating to the shorter list would leave the unmatched
    // params reading whatever their registers happen to hold.
    if (target.args.size() != target_block->params.size()) {
        report_error("Internal error: jump argument count does not match target block parameters");
        return;
    }

    // Emit MOV instructions for each block argument
    for (u32 i = 0; i < target.args.size(); i++) {
        u8 src = ensure_in_register(target.args[i].value, 0);
        u8 param_dst = get_result_register(target_block->params[i].value);

        // Check if this is a weak-typed block param (needs 2 MOVs)
        Type* param_type = target_block->params[i].type;
        u32 reg_count = get_value_reg_count(param_type);

        if (src != param_dst) {
            emit_abc(Opcode::MOV, param_dst, src, 0);
            if (reg_count > 1) {
                emit_abc(Opcode::MOV, static_cast<u8>(param_dst + 1), static_cast<u8>(src + 1), 0);
            }
        }
        spill_if_needed(target_block->params[i].value, param_dst);
    }
}

void BytecodeBuilder::lower_terminator(IRBlock* block) {
    const Terminator& term = block->terminator;

    switch (term.kind) {
        case TerminatorKind::None:
            // Should not happen in valid IR
            break;

        case TerminatorKind::Goto: {
            emit_block_args(term.goto_target);

            // Record jump for patching
            JumpPatch patch;
            patch.instruction_index = m_current_func->code.size();
            patch.target_block = term.goto_target.block;
            m_jump_patches.push_back(patch);

            // Emit jump with placeholder offset
            emit_aoff(Opcode::JMP, 0, 0);
            break;
        }

        case TerminatorKind::Branch: {
            u8 cond = ensure_in_register(term.branch.condition, 0);

            // Emit JMP_IF_NOT to skip past the then-path MOVs + JMP
            u32 jmp_if_not_idx = m_current_func->code.size();
            emit_aoff(Opcode::JMP_IF_NOT, cond, 0);  // placeholder offset

            // Emit then-branch arguments (only executes when cond is true)
            emit_block_args(term.branch.then_target);

            // Jump to then-block
            JumpPatch then_patch;
            then_patch.instruction_index = m_current_func->code.size();
            then_patch.target_block = term.branch.then_target.block;
            m_jump_patches.push_back(then_patch);
            emit_aoff(Opcode::JMP, 0, 0);

            // Patch JMP_IF_NOT to jump here (else label)
            u32 else_label = m_current_func->code.size();
            i16 skip_offset = branch_offset(jmp_if_not_idx, else_label);
            m_current_func->code[jmp_if_not_idx] = encode_aoff(Opcode::JMP_IF_NOT, cond, skip_offset);

            // Emit else-branch arguments (only executes when cond is false)
            emit_block_args(term.branch.else_target);

            // Jump to else-block
            JumpPatch else_patch;
            else_patch.instruction_index = m_current_func->code.size();
            else_patch.target_block = term.branch.else_target.block;
            m_jump_patches.push_back(else_patch);
            emit_aoff(Opcode::JMP, 0, 0);
            break;
        }

        case TerminatorKind::Return: {
            Type* ret_type = m_current_ir_func->return_type;

            if (term.return_value.is_valid()) {
                u8 ret = ensure_in_register(term.return_value, 0);

                // A weak ref is {pointer, generation} held INLINE in `ret` and
                // `ret + 1`. RET_STRUCT_SMALL is the wrong shape for it — that
                // opcode dereferences its source register — so it gets its own.
                if (ret_type && ret_type->kind == TypeKind::Weak) {
                    emit_abc(Opcode::RET_WEAK, ret, 0, 0);
                } else {
                    // Check if we're returning a struct
                    u32 slot_count = get_struct_slot_count(ret_type);
                    if (slot_count > 0 && slot_count <= 4) {
                        // Small struct: return in registers
                        emit_abc(Opcode::RET_STRUCT_SMALL, ret, static_cast<u8>(slot_count), 0);
                    } else if (slot_count > 4) {
                        // Large struct: already written to hidden out-ptr (first param)
                        // Just return void
                        emit_abc(Opcode::RET_VOID, 0, 0, 0);
                    } else {
                        // Regular return
                        emit_abc(Opcode::RET, ret, 0, 0);
                    }
                }
            } else {
                emit_abc(Opcode::RET_VOID, 0, 0, 0);
            }
            break;
        }

        case TerminatorKind::Unreachable:
            emit_abc(Opcode::TRAP, 0, 0, 0);
            break;
    }
}

i16 BytecodeBuilder::branch_offset(u32 from_idx, u32 to_idx) {
    i64 delta = static_cast<i64>(to_idx) - static_cast<i64>(from_idx) - 1;
    if (delta < -32768 || delta > 32767) {
        report_error("Branch offset out of range: function is too large "
                     "(more than 32K code words between a branch and its target)");
        return 0;
    }
    return static_cast<i16>(delta);
}

void BytecodeBuilder::patch_jumps() {
    for (const JumpPatch& patch : m_jump_patches) {
        u32 target_offset = block_offset(patch.target_block.id);
        if (target_offset == NO_OFFSET) {
            continue;  // Invalid target
        }

        i16 relative_offset = branch_offset(patch.instruction_index, target_offset);

        // Patch the instruction
        u32& instr = m_current_func->code[patch.instruction_index];
        Opcode op = decode_opcode(instr);
        u8 a = decode_a(instr);

        instr = encode_aoff(op, a, relative_offset);
    }
}

void BytecodeBuilder::fuse_compare_branch() {
    auto& code = m_current_func->code;
    if (code.size() < 2) return;

    // Map a compare opcode + branch direction (taken-on-true vs taken-on-false)
    // to its fused opcode. Returns Opcode::NOP if no fusion is available.
    auto pick_fused = [](Opcode cmp_op, bool jmp_if_not) -> Opcode {
        // Integer compares: 0x40-0x45
        if (cmp_op >= Opcode::EQ_I && cmp_op <= Opcode::GE_I) {
            if (jmp_if_not) {
                switch (cmp_op) {
                    case Opcode::EQ_I: return Opcode::JMP_IF_NE_I;
                    case Opcode::NE_I: return Opcode::JMP_IF_EQ_I;
                    case Opcode::LT_I: return Opcode::JMP_IF_GE_I;
                    case Opcode::LE_I: return Opcode::JMP_IF_GT_I;
                    case Opcode::GT_I: return Opcode::JMP_IF_LE_I;
                    case Opcode::GE_I: return Opcode::JMP_IF_LT_I;
                    default: break;
                }
            } else {
                switch (cmp_op) {
                    case Opcode::EQ_I: return Opcode::JMP_IF_EQ_I;
                    case Opcode::NE_I: return Opcode::JMP_IF_NE_I;
                    case Opcode::LT_I: return Opcode::JMP_IF_LT_I;
                    case Opcode::LE_I: return Opcode::JMP_IF_LE_I;
                    case Opcode::GT_I: return Opcode::JMP_IF_GT_I;
                    case Opcode::GE_I: return Opcode::JMP_IF_GE_I;
                    default: break;
                }
            }
        }
        // f64 non-RK: 0x56-0x5B
        if (cmp_op >= Opcode::EQ_D && cmp_op <= Opcode::GE_D) {
            if (jmp_if_not) {
                switch (cmp_op) {
                    case Opcode::EQ_D: return Opcode::JMP_IF_NE_D;
                    case Opcode::NE_D: return Opcode::JMP_IF_EQ_D;
                    case Opcode::LT_D: return Opcode::JMP_IF_GE_D;
                    case Opcode::LE_D: return Opcode::JMP_IF_GT_D;
                    case Opcode::GT_D: return Opcode::JMP_IF_LE_D;
                    case Opcode::GE_D: return Opcode::JMP_IF_LT_D;
                    default: break;
                }
            } else {
                switch (cmp_op) {
                    case Opcode::EQ_D: return Opcode::JMP_IF_EQ_D;
                    case Opcode::NE_D: return Opcode::JMP_IF_NE_D;
                    case Opcode::LT_D: return Opcode::JMP_IF_LT_D;
                    case Opcode::LE_D: return Opcode::JMP_IF_LE_D;
                    case Opcode::GT_D: return Opcode::JMP_IF_GT_D;
                    case Opcode::GE_D: return Opcode::JMP_IF_GE_D;
                    default: break;
                }
            }
        }
        // f64 RK: 0xD5-0xDA. Note negation flips inequality direction; the
        // RK constant operand stays on the right since these all read K[c].
        if (cmp_op >= Opcode::EQ_D_RK && cmp_op <= Opcode::GE_D_RK) {
            if (jmp_if_not) {
                switch (cmp_op) {
                    case Opcode::EQ_D_RK: return Opcode::JMP_IF_NE_D_RK;
                    case Opcode::NE_D_RK: return Opcode::JMP_IF_EQ_D_RK;
                    case Opcode::LT_D_RK: return Opcode::JMP_IF_GE_D_RK;
                    case Opcode::LE_D_RK: return Opcode::JMP_IF_GT_D_RK;
                    case Opcode::GT_D_RK: return Opcode::JMP_IF_LE_D_RK;
                    case Opcode::GE_D_RK: return Opcode::JMP_IF_LT_D_RK;
                    default: break;
                }
            } else {
                switch (cmp_op) {
                    case Opcode::EQ_D_RK: return Opcode::JMP_IF_EQ_D_RK;
                    case Opcode::NE_D_RK: return Opcode::JMP_IF_NE_D_RK;
                    case Opcode::LT_D_RK: return Opcode::JMP_IF_LT_D_RK;
                    case Opcode::LE_D_RK: return Opcode::JMP_IF_LE_D_RK;
                    case Opcode::GT_D_RK: return Opcode::JMP_IF_GT_D_RK;
                    case Opcode::GE_D_RK: return Opcode::JMP_IF_GE_D_RK;
                    default: break;
                }
            }
        }
        return Opcode::NOP;
    };

    // Walk instruction-by-instruction, not word-by-word: a two-word
    // instruction's payload (a CALL's function index, a GET_FIELD slot offset,
    // a padding word) must never be decoded as an opcode — a payload whose top
    // byte happened to match a compare opcode would get fused into garbage.
    u32 i = 0;
    while (i + 1 < code.size()) {
        Opcode cmp_op = decode_opcode(code[i]);
        if (is_two_word_instruction(cmp_op)) {
            i += 2;
            continue;
        }
        Opcode jmp_op = decode_opcode(code[i + 1]);

        Opcode fused_op = (jmp_op == Opcode::JMP_IF_NOT || jmp_op == Opcode::JMP_IF)
            ? pick_fused(cmp_op, jmp_op == Opcode::JMP_IF_NOT)
            : Opcode::NOP;

        // Skip compares whose result is used beyond this block's terminator.
        // Fusion drops the compare's register write, so a cross-block read would
        // get stale/uninitialized bytes.
        if (fused_op == Opcode::NOP || m_unfusable_cmp_pcs.count(i)) {
            i++;
            continue;
        }

        // The comparison destination must match the branch condition register
        u8 cmp_dst = decode_a(code[i]);
        u8 jmp_reg = decode_a(code[i + 1]);
        if (cmp_dst != jmp_reg) {
            i++;
            continue;
        }

        u8 src1 = decode_b(code[i]);
        u8 src2 = decode_c(code[i]);  // register or RK constant index
        i16 offset = decode_offset(code[i + 1]);

        // Replace: word 0 = fused opcode with src1+src2, word 1 = offset (i32)
        code[i] = encode_abc(fused_op, 0, src1, src2);
        code[i + 1] = static_cast<u32>(static_cast<i32>(offset));

        // Skip past the fused pair
        i += 2;
    }
}

u32 BytecodeBuilder::get_struct_slot_count(Type* type) const {
    if (!type || !type->is_struct()) return 0;
    return type->struct_info.slot_count;
}

u32 BytecodeBuilder::get_value_reg_count(Type* type) const {
    if (!type) return 1;
    if (type->kind == TypeKind::Weak) return 2;  // 128-bit: pointer + generation
    u32 slot_count = get_struct_slot_count(type);
    return (slot_count > 0 && slot_count <= 4) ? (slot_count + 1) / 2 : 1;
}

u32 BytecodeBuilder::call_return_extra_regs(Type* type) const {
    if (!type) return 0;
    if (type->kind == TypeKind::Weak) return 2;
    u32 slot_count = get_struct_slot_count(type);
    return (slot_count > 0 && slot_count <= 4) ? (slot_count + 1) / 2 : 0;
}

Opcode BytecodeBuilder::get_opcode(IROp op) const {
    switch (op) {
        // Integer arithmetic
        case IROp::AddI:    return Opcode::ADD_I;
        case IROp::SubI:    return Opcode::SUB_I;
        case IROp::MulI:    return Opcode::MUL_I;
        case IROp::DivI:    return Opcode::DIV_I;
        case IROp::ModI:    return Opcode::MOD_I;
        case IROp::DivU:    return Opcode::DIV_U;
        case IROp::ModU:    return Opcode::MOD_U;
        case IROp::NegI:    return Opcode::NEG_I;

        // f32 arithmetic
        case IROp::AddF:    return Opcode::ADD_F;
        case IROp::SubF:    return Opcode::SUB_F;
        case IROp::MulF:    return Opcode::MUL_F;
        case IROp::DivF:    return Opcode::DIV_F;
        case IROp::NegF:    return Opcode::NEG_F;

        // f64 arithmetic
        case IROp::AddD:    return Opcode::ADD_D;
        case IROp::SubD:    return Opcode::SUB_D;
        case IROp::MulD:    return Opcode::MUL_D;
        case IROp::DivD:    return Opcode::DIV_D;
        case IROp::NegD:    return Opcode::NEG_D;

        // Integer comparisons
        case IROp::EqI:     return Opcode::EQ_I;
        case IROp::NeI:     return Opcode::NE_I;
        case IROp::LtI:     return Opcode::LT_I;
        case IROp::LeI:     return Opcode::LE_I;
        case IROp::GtI:     return Opcode::GT_I;
        case IROp::GeI:     return Opcode::GE_I;

        // Unsigned integer comparisons
        case IROp::LtU:     return Opcode::LT_U;
        case IROp::LeU:     return Opcode::LE_U;
        case IROp::GtU:     return Opcode::GT_U;
        case IROp::GeU:     return Opcode::GE_U;

        // f32 comparisons
        case IROp::EqF:     return Opcode::EQ_F;
        case IROp::NeF:     return Opcode::NE_F;
        case IROp::LtF:     return Opcode::LT_F;
        case IROp::LeF:     return Opcode::LE_F;
        case IROp::GtF:     return Opcode::GT_F;
        case IROp::GeF:     return Opcode::GE_F;

        // f64 comparisons
        case IROp::EqD:     return Opcode::EQ_D;
        case IROp::NeD:     return Opcode::NE_D;
        case IROp::LtD:     return Opcode::LT_D;
        case IROp::LeD:     return Opcode::LE_D;
        case IROp::GtD:     return Opcode::GT_D;
        case IROp::GeD:     return Opcode::GE_D;

        // Logical. And/Or route through bitwise ops because Roxy's bool
        // representation is normalized 0/1 (LOAD_TRUE/FALSE write 1/0,
        // comparison opcodes use reg_from_bool, NOT/I_TO_B produce 0/1).
        // BIT_AND/BIT_OR on 0/1 give identical results to logical && / ||.
        // The only IR producer of IROp::Or is when-statement variant combining
        // (ir_builder.cpp gen_when_stmt), which feeds it EqI results — always
        // 0 or 1. Source-level && and || lower to short-circuit branches in
        // gen_binary_expr and never reach this opcode mapping.
        case IROp::Not:     return Opcode::NOT;
        case IROp::And:     return Opcode::BIT_AND;
        case IROp::Or:      return Opcode::BIT_OR;

        // Bitwise
        case IROp::BitAnd:  return Opcode::BIT_AND;
        case IROp::BitOr:   return Opcode::BIT_OR;
        case IROp::BitXor:  return Opcode::BIT_XOR;
        case IROp::BitNot:  return Opcode::BIT_NOT;
        case IROp::Shl:     return Opcode::SHL;
        case IROp::Shr:     return Opcode::SHR;
        case IROp::UShr:    return Opcode::USHR;

        // Type conversions
        case IROp::I_TO_F64:  return Opcode::I_TO_F64;
        case IROp::F64_TO_I:  return Opcode::F64_TO_I;
        case IROp::I_TO_B:    return Opcode::I_TO_B;
        case IROp::B_TO_I:    return Opcode::B_TO_I;

        default:
            return Opcode::NOP;
    }
}

// Helper to get bit width of an integer type
static u8 get_int_bits(TypeKind kind) {
    switch (kind) {
        case TypeKind::I8:  case TypeKind::U8:  return 8;
        case TypeKind::I16: case TypeKind::U16: return 16;
        case TypeKind::I32: case TypeKind::U32: return 32;
        case TypeKind::I64: case TypeKind::U64: return 64;
        default: return 64;  // Default to 64-bit
    }
}

static bool is_signed_type(TypeKind kind) {
    return kind == TypeKind::I8 || kind == TypeKind::I16 ||
           kind == TypeKind::I32 || kind == TypeKind::I64;
}

void BytecodeBuilder::emit_cast_bytecode(u8 dst, u8 src, Type* source_type, Type* target_type) {
    if (!source_type || !target_type) {
        emit_abc(Opcode::MOV, dst, src, 0);
        return;
    }

    TypeKind src_kind = source_type->kind;
    TypeKind tgt_kind = target_type->kind;

    // Same type: just MOV
    if (src_kind == tgt_kind) {
        if (dst != src) {
            emit_abc(Opcode::MOV, dst, src, 0);
        }
        return;
    }

    // Any type to bool: use I_TO_B (normalizes to 0/1)
    if (tgt_kind == TypeKind::Bool) {
        emit_abc(Opcode::I_TO_B, dst, src, 0);
        return;
    }

    // Bool to anything: MOV is sufficient since bool is already 0/1
    if (src_kind == TypeKind::Bool) {
        // Bool to integer: just MOV (already 0 or 1)
        if (target_type->is_integer()) {
            if (dst != src) {
                emit_abc(Opcode::MOV, dst, src, 0);
            }
            return;
        }
        // Bool to f64
        if (tgt_kind == TypeKind::F64) {
            emit_abc(Opcode::I_TO_F64, dst, src, 0);
            return;
        }
        // Bool to f32
        if (tgt_kind == TypeKind::F32) {
            emit_abc(Opcode::I_TO_F32, dst, src, 0);
            return;
        }
    }

    // Float conversions
    if (source_type->is_float() && target_type->is_float()) {
        if (src_kind == TypeKind::F32 && tgt_kind == TypeKind::F64) {
            emit_abc(Opcode::F32_TO_F64, dst, src, 0);
        } else {
            emit_abc(Opcode::F64_TO_F32, dst, src, 0);
        }
        return;
    }

    // Integer to float
    if (source_type->is_integer() && target_type->is_float()) {
        if (tgt_kind == TypeKind::F64) {
            emit_abc(Opcode::I_TO_F64, dst, src, 0);
        } else {
            emit_abc(Opcode::I_TO_F32, dst, src, 0);
        }
        return;
    }

    // Float to integer
    if (source_type->is_float() && target_type->is_integer()) {
        u8 temp = dst;
        if (src_kind == TypeKind::F32) {
            emit_abc(Opcode::F32_TO_I, temp, src, 0);
        } else {
            emit_abc(Opcode::F64_TO_I, temp, src, 0);
        }
        // If target is smaller than i64, truncate
        u8 tgt_bits = get_int_bits(tgt_kind);
        if (tgt_bits < 64) {
            if (is_signed_type(tgt_kind)) {
                emit_abc(Opcode::TRUNC_S, dst, temp, tgt_bits);
            } else {
                emit_abc(Opcode::TRUNC_U, dst, temp, tgt_bits);
            }
        } else if (dst != temp) {
            emit_abc(Opcode::MOV, dst, temp, 0);
        }
        return;
    }

    // Integer to integer
    if (source_type->is_integer() && target_type->is_integer()) {
        u8 src_bits = get_int_bits(src_kind);
        u8 tgt_bits = get_int_bits(tgt_kind);

        if (tgt_bits < src_bits) {
            // Narrowing: truncate
            if (is_signed_type(tgt_kind)) {
                emit_abc(Opcode::TRUNC_S, dst, src, tgt_bits);
            } else {
                emit_abc(Opcode::TRUNC_U, dst, src, tgt_bits);
            }
        } else if (tgt_bits > src_bits) {
            // Widening: value is already properly represented in 64-bit register
            // Just need to potentially sign-extend from the source width
            if (is_signed_type(src_kind)) {
                // Source is signed, need to sign-extend from src_bits to full 64-bit
                // The TRUNC_S op will sign-extend from the specified bit width
                emit_abc(Opcode::TRUNC_S, dst, src, src_bits);
            } else {
                // Source is unsigned, value is already zero-extended
                if (dst != src) {
                    emit_abc(Opcode::MOV, dst, src, 0);
                }
            }
        } else {
            // Same bit width, different signedness: just MOV
            if (dst != src) {
                emit_abc(Opcode::MOV, dst, src, 0);
            }
        }
        return;
    }

    // Fallback: just MOV
    if (dst != src) {
        emit_abc(Opcode::MOV, dst, src, 0);
    }
}

void BytecodeBuilder::build_struct_field_deletes(Type* struct_type,
                                                 u16& out_start, u16& out_count) {
    const StructTypeInfo& struct_info = struct_type->struct_info;

    // Build this struct's actions into a local buffer first. Recursive
    // build_delete_desc() calls below may append OTHER structs' field actions to
    // the shared struct_field_deletes vector, so we only flush ours contiguously
    // once all recursion has finished — keeping our [start, count) range intact.
    Vector<BCStructFieldDelete> local;

    auto append_field = [&](u32 slot_offset, Type* field_type,
                            u32 disc_slot_offset, i32 disc_value) {
        BCStructFieldDelete action;
        action.disc_value = disc_value;
        action.slot_offset = static_cast<u16>(slot_offset);
        action.field_desc_idx = build_delete_desc(field_type);
        action.disc_slot_offset = static_cast<u16>(disc_slot_offset);
        action._pad = 0;
        local.push_back(action);
    };

    // Regular owned fields (reverse declaration order = LIFO, like emit_field_cleanup).
    for (i32 i = static_cast<i32>(struct_info.fields.size()) - 1; i >= 0; i--) {
        const FieldInfo& field = struct_info.fields[i];
        if (!field.type) continue;
        // member_needs_drop covers uniq/noncopyable fields and — newly — `ref`
        // fields (a counted borrow → ref_dec on drop; lifetimes.md "Value lifecycle").
        // Non-recursive (cycle-safe): nested owning value-structs already carry a
        // synthesized destructor, so noncopyable() flags them.
        if (member_needs_drop(field.type)) {
            append_field(field.slot_offset, field.type, 0xFFFF, 0);
        }
    }

    // Tagged-union variant fields: each is guarded by the clause discriminant.
    for (const auto& clause : struct_info.when_clauses) {
        for (const auto& variant : clause.variants) {
            for (i32 fi = static_cast<i32>(variant.fields.size()) - 1; fi >= 0; fi--) {
                const VariantFieldInfo& variant_field = variant.fields[fi];
                if (!variant_field.type) continue;
                if (member_needs_drop(variant_field.type)) {
                    append_field(clause.union_slot_offset + variant_field.slot_offset,
                                 variant_field.type,
                                 clause.discriminant_slot_offset,
                                 static_cast<i32>(variant.discriminant_value));
                }
            }
        }
    }

    out_start = static_cast<u16>(m_current_func->struct_field_deletes.size());
    for (const auto& action : local) {
        m_current_func->struct_field_deletes.push_back(action);
    }
    out_count = static_cast<u16>(local.size());
}

u16 BytecodeBuilder::lookup_destructor_index(Type* struct_type) const {
    // Transient lookup key — mangle_destructor_owned grows for arbitrarily long
    // struct names (deeply monomorphized generics) instead of truncating into a
    // fixed buffer. BytecodeBuilder has no arena, so this uses the owned-String form.
    String dtor_name = mangle_destructor_owned(struct_type->struct_info.name);
    auto it = m_func_indices.find(StringView(dtor_name.data(), dtor_name.size()));
    return it != m_func_indices.end() ? static_cast<u16>(it->second) : 0;
}

u16 BytecodeBuilder::build_delete_desc(Type* type) {
    auto cached = m_delete_desc_cache.find(type);
    if (cached != m_delete_desc_cache.end()) return cached->second;

    // Reserve this type's descriptor slot BEFORE recursing into its fields, so a
    // self-referential struct (e.g. `next: uniq Node` inside Node) resolves to
    // this same index on the recursive call instead of looping forever.
    u16 index = static_cast<u16>(m_current_func->delete_descs.size());
    m_current_func->delete_descs.push_back(BCDeleteDesc{});  // placeholder, filled below
    m_delete_desc_cache[type] = index;

    // The *kind* of drop is decided once by the shared, backend-agnostic
    // compute_drop_plan (lifetimes.md "Value lifecycle"); this is the VM lowering of that
    // plan into a BCDeleteDesc. The recursion into element/field types stays here
    // (build_delete_desc / build_struct_field_deletes), and dtor references resolve
    // to bytecode function indices. The C backend lowers the same plan to glue.
    BCDeleteDesc desc;
    DropPlan plan = compute_drop_plan(type);
    desc.free_obj = plan.free_obj;
    switch (plan.kind) {
        case DropKind::None:
            break;  // inert; free_obj may still be set (uniq of a primitive)
        case DropKind::CallDtor:
            desc.cleanup = BCDeleteDesc::CallDtor;
            desc.dtor_fn_idx = lookup_destructor_index(plan.struct_type);
            break;
        case DropKind::WalkFields:
            desc.cleanup = BCDeleteDesc::WalkFields;
            build_struct_field_deletes(plan.struct_type, desc.fields.field_start, desc.fields.field_count);
            break;
        case DropKind::List:
            desc.cleanup = BCDeleteDesc::List;
            desc.container.elem_desc_idx = build_delete_desc(plan.elem_type);
            desc.container.key_desc_idx = 0xFFFF;  // unused for lists
            break;
        case DropKind::Map:
            desc.cleanup = BCDeleteDesc::Map;
            // A `ref V` value is count-bearing (shared condition); keys can't be ref.
            desc.container.elem_desc_idx =
                member_needs_drop(plan.elem_type) ? build_delete_desc(plan.elem_type) : 0xFFFF;
            desc.container.key_desc_idx = map_key_needs_drop(plan.key_type)
                    ? build_delete_desc(plan.key_type) : 0xFFFF;
            break;
        case DropKind::Closure:
            // Type-erased: cleanup dispatches the env's synthesized destructor at
            // runtime by the env's type_id, then frees it.
            desc.cleanup = BCDeleteDesc::Closure;
            break;
        case DropKind::RefDec:
            // A counted borrow: release the count (ref_dec), never free the pointee.
            desc.cleanup = BCDeleteDesc::RefDec;
            break;
        case DropKind::StrRelease:
            // An owned reference-counted string: release (free at zero). free_obj
            // stays false — release itself frees, so the descriptor walk must not
            // also object_free the pointer (finding 9b).
            desc.cleanup = BCDeleteDesc::StrRelease;
            break;
    }

    // Cross-check (lifetimes.md "Value lifecycle"): the structural
    // `needs_drop()` predicate must be at least as inclusive as this descriptor —
    // anything the current machinery actually cleans, the predicate must also flag.
    // (The reverse may differ: `needs_drop()` additionally reports `ref` struct
    // fields, a gap this descriptor doesn't yet handle — so we only assert one
    // direction.)
    bool desc_does_something = desc.cleanup != BCDeleteDesc::None || desc.free_obj;
    assert((!desc_does_something || type->needs_drop()) &&
           "needs_drop() predicate weaker than delete descriptor");

    m_current_func->delete_descs[index] = desc;
    return index;
}

}

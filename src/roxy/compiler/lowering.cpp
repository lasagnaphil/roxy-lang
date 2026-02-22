#include "roxy/compiler/lowering.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/natives.hpp"

#include <cassert>

namespace rx {

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
                                       const tsl::robin_map<u32, Type*>& value_types,
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
            auto type_it = value_types.find(arg_val.id);
            Type* arg_type = (type_it != value_types.end()) ? type_it->second : nullptr;
            u32 arg_slot_count = get_struct_slot_count_fn(arg_type);
            if (arg_slot_count > 0 && arg_slot_count <= 4) {
                arg_reg_count += (arg_slot_count + 1) / 2;
            } else {
                arg_reg_count += 1;
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

    // Reset state
    m_value_to_reg.clear();
    m_value_to_stack_slot.clear();
    m_var_name_to_stack_slot.clear();
    m_value_types.clear();
    m_block_offsets.clear();
    m_jump_patches.clear();
    m_free_regs.clear();
    m_active.clear();
    m_rpo_order.clear();
    m_next_reg = 0;
    m_next_stack_slot = 0;

    // Step 1: Compute RPO ordering and liveness intervals for all SSA values
    compute_rpo(ir_func);
    compute_liveness(ir_func);

    // Step 2: Allocate registers for function parameters (pre-colored)
    u8 param_reg_offset = 0;
    for (u32 i = 0; i < ir_func->params.size(); i++) {
        const auto& param = ir_func->params[i];

        // Map this parameter value to its starting register
        m_value_to_reg[param.value.id] = param_reg_offset;
        if (param.type) {
            m_value_types[param.value.id] = param.type;
        }

        // Check if this parameter is a pointer (out/inout)
        bool is_ptr_param = (i < ir_func->param_is_ptr.size() && ir_func->param_is_ptr[i]);

        // Calculate how many registers this parameter uses
        u8 reg_count = 1;
        if (!is_ptr_param) {
            u32 slot_count = get_struct_slot_count(param.type);
            if (slot_count > 0 && slot_count <= 4) {
                reg_count = static_cast<u8>((slot_count + 1) / 2);
            }
        }

        // Add parameter to active set so it can expire when no longer used
        if (param.value.id < m_live_ranges.size()) {
            u32 last_use = m_live_ranges[param.value.id].last_use_point;
            // Add each register used by this parameter to the active set
            for (u8 r = 0; r < reg_count; r++) {
                // Insert into m_active sorted by last_use
                ActiveAlloc alloc{last_use, static_cast<u8>(param_reg_offset + r)};
                auto* pos = m_active.find_if([&](const ActiveAlloc& a) { return a.last_use > last_use; });
                if (pos) {
                    m_active.insert(pos, alloc);
                } else {
                    m_active.push_back(alloc);
                }
            }
        }

        param_reg_offset += reg_count;
    }
    m_next_reg = param_reg_offset;
    m_current_func->param_register_count = param_reg_offset;

    // Step 3: Liveness-aware pre-allocation of all SSA values (RPO order)
    {
        u32 alloc_point = 0;
        for (u32 rpo_idx : m_rpo_order) {
            IRBlock* block = ir_func->blocks[rpo_idx];

            // Block parameters
            for (const auto& param : block->params) {
                expire_before(alloc_point);
                if (!has_register(param.value)) {
                    allocate_register(param.value);
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
                                inst->op == IROp::CallExternal);

                if (inst->result.is_valid() && !has_register(inst->result)) {
                    if (is_call) {
                        // Call results must use bump (not free list) because the calling
                        // convention needs a contiguous block [dst, dst+1, ...] for args.
                        // A free-list register could have live values in subsequent slots.
                        u8 reg = bump_register();
                        m_value_to_reg[inst->result.id] = reg;
                    } else {
                        allocate_register(inst->result);
                    }
                }
                if (inst->result.is_valid() && inst->type) {
                    m_value_types[inst->result.id] = inst->type;
                }

                // For calls, reserve contiguous registers for args and struct returns
                if ((inst->op == IROp::Call || inst->op == IROp::CallNative) && inst->result.is_valid()) {
                    u8 dst = get_register(inst->result);
                    u32 extra_regs_for_return = 0;
                    u32 ret_slot_count = get_struct_slot_count(inst->type);
                    if (ret_slot_count > 0 && ret_slot_count <= 4) {
                        extra_regs_for_return = (ret_slot_count + 1) / 2;
                    }

                    // Compute actual arg register count based on types
                    StringView func_name = inst->call.func_name;
                    IRFunction* callee_func = nullptr;
                    auto func_it = m_func_indices.find(func_name);
                    if (func_it != m_func_indices.end()) {
                        callee_func = m_ir_module->functions[func_it->second];
                    }
                    u32 total_arg_regs = compute_call_arg_reg_count(inst, callee_func, m_value_types,
                        [this](Type* t) { return get_struct_slot_count(t); });

                    u16 needed_regs = static_cast<u16>(dst) + static_cast<u16>(extra_regs_for_return) + 1 + static_cast<u16>(total_arg_regs);
                    while (m_next_reg < needed_regs) {
                        bump_register();
                    }
                }

                if (inst->op == IROp::CallExternal && inst->result.is_valid()) {
                    u8 dst = get_register(inst->result);
                    u32 extra_regs_for_return = 0;
                    u32 ret_slot_count = get_struct_slot_count(inst->type);
                    if (ret_slot_count > 0 && ret_slot_count <= 4) {
                        extra_regs_for_return = (ret_slot_count + 1) / 2;
                    }

                    StringView func_name = inst->call_external.func_name;
                    IRFunction* callee_func = nullptr;
                    auto func_it = m_func_indices.find(func_name);
                    if (func_it != m_func_indices.end()) {
                        callee_func = m_ir_module->functions[func_it->second];
                    }
                    u32 total_arg_regs = compute_call_arg_reg_count(inst, callee_func, m_value_types,
                        [this](Type* t) { return get_struct_slot_count(t); });

                    u16 needed_regs = static_cast<u16>(dst) + static_cast<u16>(extra_regs_for_return) + 1 + static_cast<u16>(total_arg_regs);
                    while (m_next_reg < needed_regs) {
                        bump_register();
                    }
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
                        allocate_register(param.value);
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
        if (m_registry && param.type && (param.type->is_list() || param.type->is_map())) {
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
                    emit_abc(Opcode::CALL_NATIVE, copy_dst, static_cast<u8>(copy_fn_idx), 1);
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
            m_value_to_stack_slot[param.value.id] = stack_offset;

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
            m_value_to_stack_slot[param.value.id] = stack_offset;

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

    // Second pass: emit bytecode in RPO order
    for (u32 rpo_idx : m_rpo_order) {
        IRBlock* block = ir_func->blocks[rpo_idx];

        // Record block offset
        m_block_offsets[block->id.id] = m_current_func->code.size();

        // Lower all instructions
        for (IRInst* inst : block->instructions) {
            lower_instruction(inst);
        }

        // Lower terminator
        lower_terminator(block);
    }

    // Patch jump offsets
    patch_jumps();

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

u8 BytecodeBuilder::allocate_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    auto it = m_value_to_reg.find(value.id);
    if (it != m_value_to_reg.end()) {
        return it->second;
    }

    // Determine if this value can reuse a freed register.
    // Cross-block values must always get fresh registers because the IR may have
    // partially-defined values (e.g., AND/OR short-circuit) where a value is only
    // defined on one branch. Fresh registers are zero-initialized by the VM.
    bool can_reuse = (value.id < m_value_same_block.size() && m_value_same_block[value.id]);

    u8 reg;
    if (can_reuse && !m_free_regs.empty()) {
        // Find the smallest available register for deterministic allocation
        u32 min_idx = 0;
        for (u32 i = 1; i < m_free_regs.size(); i++) {
            if (m_free_regs[i] < m_free_regs[min_idx]) {
                min_idx = i;
            }
        }
        reg = m_free_regs[min_idx];
        // Remove by swapping with last element
        m_free_regs[min_idx] = m_free_regs.back();
        m_free_regs.pop_back();
    } else {
        reg = bump_register();
    }

    m_value_to_reg[value.id] = reg;

    // Add to active set for expiry tracking.
    // With RPO ordering, liveness is correct for all values including block params.
    // Block params still get fresh registers (can_reuse = false because they're
    // cross-block), but they ARE freed when dead.
    if (value.id < m_live_ranges.size()) {
        u32 last_use = m_live_ranges[value.id].last_use_point;
        ActiveAlloc alloc{last_use, reg};
        auto* pos = m_active.find_if([&](const ActiveAlloc& a) { return a.last_use > last_use; });
        if (pos) {
            m_active.insert(pos, alloc);
        } else {
            m_active.push_back(alloc);
        }
    }

    return reg;
}

u8 BytecodeBuilder::get_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    auto it = m_value_to_reg.find(value.id);
    if (it == m_value_to_reg.end()) {
        report_error("Internal error: SSA value used before allocation");
        return 0xFF;
    }
    return it->second;
}

bool BytecodeBuilder::has_register(ValueId value) const {
    if (!value.is_valid()) return false;
    return m_value_to_reg.find(value.id) != m_value_to_reg.end();
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

void BytecodeBuilder::compute_rpo(IRFunction* ir_func) {
    u32 num_blocks = ir_func->blocks.size();
    m_rpo_order.clear();
    if (num_blocks == 0) return;

    // Iterative DFS to compute reverse postorder.
    // Phase 0 = visit children, phase 1 = emit to post-order.
    Vector<bool> visited;
    visited.reserve(num_blocks);
    for (u32 i = 0; i < num_blocks; i++) visited.push_back(false);

    struct StackEntry { u32 block_idx; u8 phase; };
    Vector<StackEntry> stack;
    stack.push_back({0, 0});
    visited[0] = true;

    Vector<u32> post_order;

    while (!stack.empty()) {
        auto& entry = stack.back();
        if (entry.phase == 1) {
            post_order.push_back(entry.block_idx);
            stack.pop_back();
            continue;
        }
        // Phase 0: mark for emit, then push successors
        entry.phase = 1;

        IRBlock* block = ir_func->blocks[entry.block_idx];
        const Terminator& term = block->terminator;

        // Push successors in reverse order so they come out in forward order
        auto push_successor = [&](BlockId target_id) {
            if (!target_id.is_valid() || target_id.id >= num_blocks) return;
            if (!visited[target_id.id]) {
                visited[target_id.id] = true;
                stack.push_back({target_id.id, 0});
            }
        };

        switch (term.kind) {
            case TerminatorKind::Goto:
                push_successor(term.goto_target.block);
                break;
            case TerminatorKind::Branch:
                // Push else first, then then — so then is processed first (top of stack)
                push_successor(term.branch.else_target.block);
                push_successor(term.branch.then_target.block);
                break;
            default:
                break;
        }
    }

    // Reverse for RPO
    for (i32 i = static_cast<i32>(post_order.size()) - 1; i >= 0; i--) {
        m_rpo_order.push_back(post_order[i]);
    }

    // Append any unreachable blocks defensively
    for (u32 i = 0; i < num_blocks; i++) {
        if (!visited[i]) {
            m_rpo_order.push_back(i);
        }
    }
}

void BytecodeBuilder::compute_liveness(IRFunction* ir_func) {
    // Allocate live ranges for all SSA values in this function
    u32 num_values = ir_func->next_value_id;
    m_live_ranges.clear();
    m_live_ranges.reserve(num_values);
    for (u32 i = 0; i < num_values; i++) {
        m_live_ranges.push_back(LiveRange{0, 0});
    }

    // Pass 1: assign definition points (RPO order)
    u32 point = 0;
    for (u32 rpo_idx : m_rpo_order) {
        IRBlock* block = ir_func->blocks[rpo_idx];
        for (const auto& param : block->params) {
            if (param.value.is_valid() && param.value.id < num_values) {
                m_live_ranges[param.value.id].def_point = point;
                m_live_ranges[param.value.id].last_use_point = point;  // at least live at def
            }
            point++;
        }
        for (IRInst* inst : block->instructions) {
            if (inst->result.is_valid() && inst->result.id < num_values) {
                m_live_ranges[inst->result.id].def_point = point;
                m_live_ranges[inst->result.id].last_use_point = point;  // at least live at def
            }
            point++;
        }
        point++;  // terminator slot
    }

    // Pass 2: scan operands to find last uses (RPO order)
    point = 0;
    for (u32 rpo_idx : m_rpo_order) {
        IRBlock* block = ir_func->blocks[rpo_idx];
        // Skip block param slots (they are definitions, not uses)
        point += block->params.size();

        for (IRInst* inst : block->instructions) {
            // Extract operands based on op type
            switch (inst->op) {
                // Binary ops
                case IROp::AddI: case IROp::SubI: case IROp::MulI: case IROp::DivI: case IROp::ModI:
                case IROp::AddF: case IROp::SubF: case IROp::MulF: case IROp::DivF:
                case IROp::AddD: case IROp::SubD: case IROp::MulD: case IROp::DivD:
                case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor: case IROp::Shl: case IROp::Shr:
                case IROp::EqI: case IROp::NeI: case IROp::LtI: case IROp::LeI: case IROp::GtI: case IROp::GeI:
                case IROp::EqF: case IROp::NeF: case IROp::LtF: case IROp::LeF: case IROp::GtF: case IROp::GeF:
                case IROp::EqD: case IROp::NeD: case IROp::LtD: case IROp::LeD: case IROp::GtD: case IROp::GeD:
                case IROp::And: case IROp::Or:
                    mark_use(m_live_ranges, inst->binary.left, point);
                    mark_use(m_live_ranges, inst->binary.right, point);
                    break;

                // Unary ops
                case IROp::NegI: case IROp::NegF: case IROp::NegD:
                case IROp::BitNot: case IROp::Not:
                case IROp::I_TO_F64: case IROp::F64_TO_I: case IROp::I_TO_B: case IROp::B_TO_I:
                case IROp::Copy:
                case IROp::RefInc: case IROp::RefDec: case IROp::WeakCheck:
                case IROp::Delete:
                    mark_use(m_live_ranges, inst->unary, point);
                    break;

                // Call / CallNative
                case IROp::Call:
                case IROp::CallNative:
                    for (u32 i = 0; i < inst->call.args.size(); i++) {
                        mark_use(m_live_ranges, inst->call.args[i], point);
                    }
                    break;

                // CallExternal
                case IROp::CallExternal:
                    for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                        mark_use(m_live_ranges, inst->call_external.args[i], point);
                    }
                    break;

                // Field access
                case IROp::GetField:
                case IROp::GetFieldAddr:
                    mark_use(m_live_ranges, inst->field.object, point);
                    break;

                case IROp::SetField:
                    mark_use(m_live_ranges, inst->field.object, point);
                    mark_use(m_live_ranges, inst->store_value, point);
                    break;

                // Struct copy
                case IROp::StructCopy:
                    mark_use(m_live_ranges, inst->struct_copy.dest_ptr, point);
                    mark_use(m_live_ranges, inst->struct_copy.source_ptr, point);
                    break;

                // Pointer ops
                case IROp::LoadPtr:
                    mark_use(m_live_ranges, inst->load_ptr.ptr, point);
                    break;

                case IROp::StorePtr:
                    mark_use(m_live_ranges, inst->store_ptr.ptr, point);
                    mark_use(m_live_ranges, inst->store_ptr.value, point);
                    break;

                // Cast
                case IROp::Cast:
                    mark_use(m_live_ranges, inst->cast.source, point);
                    break;

                // New (args)
                case IROp::New:
                    for (u32 i = 0; i < inst->new_data.args.size(); i++) {
                        mark_use(m_live_ranges, inst->new_data.args[i], point);
                    }
                    break;

                // No operands
                case IROp::ConstNull: case IROp::ConstBool: case IROp::ConstInt:
                case IROp::ConstF: case IROp::ConstD: case IROp::ConstString:
                case IROp::StackAlloc: case IROp::BlockArg: case IROp::VarAddr:
                    break;
            }
            point++;
        }

        // Terminator operands
        const Terminator& term = block->terminator;
        switch (term.kind) {
            case TerminatorKind::Goto:
                for (u32 i = 0; i < term.goto_target.args.size(); i++) {
                    mark_use(m_live_ranges, term.goto_target.args[i].value, point);
                }
                break;

            case TerminatorKind::Branch:
                mark_use(m_live_ranges, term.branch.condition, point);
                for (u32 i = 0; i < term.branch.then_target.args.size(); i++) {
                    mark_use(m_live_ranges, term.branch.then_target.args[i].value, point);
                }
                for (u32 i = 0; i < term.branch.else_target.args.size(); i++) {
                    mark_use(m_live_ranges, term.branch.else_target.args[i].value, point);
                }
                break;

            case TerminatorKind::Return:
                mark_use(m_live_ranges, term.return_value, point);
                break;

            case TerminatorKind::None:
            case TerminatorKind::Unreachable:
                break;
        }
        point++;  // terminator slot
    }

    // Pass 3: extend block param live ranges to cover predecessor terminators
    // This prevents the register allocator from reusing block param registers
    // before all MOVs for block args have been emitted (parallel assignment safety).
    // For each jump target with args, find the target block's params and extend
    // their live range to include the predecessor's terminator point.
    point = 0;
    for (u32 rpo_idx : m_rpo_order) {
        IRBlock* block = ir_func->blocks[rpo_idx];
        point += block->params.size();
        point += block->instructions.size();
        u32 terminator_point = point;

        auto extend_target_params = [&](const JumpTarget& target) {
            if (!target.block.is_valid() || target.block.id >= ir_func->blocks.size()) return;
            IRBlock* target_block = ir_func->blocks[target.block.id];
            // Extend each param's live range to cover this terminator
            for (u32 i = 0; i < target_block->params.size(); i++) {
                mark_use(m_live_ranges, target_block->params[i].value, terminator_point);
            }
        };

        const Terminator& term = block->terminator;
        switch (term.kind) {
            case TerminatorKind::Goto:
                if (term.goto_target.args.size() > 0) {
                    extend_target_params(term.goto_target);
                }
                break;
            case TerminatorKind::Branch:
                if (term.branch.then_target.args.size() > 0) {
                    extend_target_params(term.branch.then_target);
                }
                if (term.branch.else_target.args.size() > 0) {
                    extend_target_params(term.branch.else_target);
                }
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
    // (indexed by RPO position, using RPO ordering for back-edge detection)
    struct BlockPointInfo {
        u32 first_point;
        u32 term_point;
    };
    Vector<BlockPointInfo> block_points;
    tsl::robin_map<u32, u32> block_id_to_rpo;  // BlockId.id -> RPO position
    {
        u32 bp = 0;
        for (u32 rpo_pos = 0; rpo_pos < m_rpo_order.size(); rpo_pos++) {
            IRBlock* blk = ir_func->blocks[m_rpo_order[rpo_pos]];
            u32 first = bp;
            bp += blk->params.size();
            bp += blk->instructions.size();
            u32 term = bp;
            bp++;
            block_points.push_back({first, term});
            block_id_to_rpo[blk->id.id] = rpo_pos;
        }
    }

    // Fixed-point iteration for nested loops
    bool changed = true;
    while (changed) {
        changed = false;
        for (u32 rpo_pos = 0; rpo_pos < m_rpo_order.size(); rpo_pos++) {
            IRBlock* blk = ir_func->blocks[m_rpo_order[rpo_pos]];

            auto check_back_edge = [&](BlockId target_id) {
                auto it = block_id_to_rpo.find(target_id.id);
                if (it == block_id_to_rpo.end()) return;
                u32 target_rpo_pos = it->second;
                if (target_rpo_pos >= rpo_pos) return;  // Not a back edge

                // Back edge from rpo_pos to target_rpo_pos
                u32 loop_start = block_points[target_rpo_pos].first_point;
                u32 loop_end = block_points[rpo_pos].term_point;

                // Extend values defined before the loop but used inside it
                for (u32 vi = 0; vi < num_values; vi++) {
                    auto& lr = m_live_ranges[vi];
                    if (lr.def_point < loop_start &&
                        lr.last_use_point >= loop_start &&
                        lr.last_use_point < loop_end) {
                        lr.last_use_point = loop_end;
                        changed = true;
                    }
                }
            };

            const Terminator& term = blk->terminator;
            switch (term.kind) {
                case TerminatorKind::Goto:
                    check_back_edge(term.goto_target.block);
                    break;
                case TerminatorKind::Branch:
                    check_back_edge(term.branch.then_target.block);
                    check_back_edge(term.branch.else_target.block);
                    break;
                default:
                    break;
            }
        }
    }

    // Compute same-block flags: a value is same-block if its def_point and
    // last_use_point fall within the same block's range.
    // Cross-block values (used in a different block than defined) must NOT
    // have their registers reused from the free list, because the IR may have
    // partially-defined values (e.g., AND/OR short-circuit patterns where a
    // value is only defined on one branch). Fresh registers are zero-initialized
    // by the VM, preserving correct behavior for such patterns.
    m_value_same_block.clear();
    m_value_same_block.reserve(num_values);
    for (u32 vi = 0; vi < num_values; vi++) {
        m_value_same_block.push_back(false);
    }
    for (u32 rpo_pos = 0; rpo_pos < m_rpo_order.size(); rpo_pos++) {
        u32 block_start = block_points[rpo_pos].first_point;
        u32 block_end = block_points[rpo_pos].term_point;
        for (u32 vi = 0; vi < num_values; vi++) {
            auto& lr = m_live_ranges[vi];
            if (lr.def_point >= block_start && lr.def_point <= block_end &&
                lr.last_use_point >= block_start && lr.last_use_point <= block_end) {
                m_value_same_block[vi] = true;
            }
        }
    }

    // Force block params cross-block.
    // Block params receive values from predecessor blocks, so they must not
    // reuse freed registers (need fresh zero-initialized regs).
    // With RPO ordering, their liveness is now correct, so they CAN be freed
    // after their last use (unlike before where they were permanently pinned).
    for (u32 rpo_idx : m_rpo_order) {
        IRBlock* block = ir_func->blocks[rpo_idx];
        for (const auto& param : block->params) {
            if (param.value.is_valid() && param.value.id < num_values) {
                m_value_same_block[param.value.id] = false;
            }
        }
    }
}

void BytecodeBuilder::expire_before(u32 current_point) {
    // Pop all entries from the front of m_active whose last_use < current_point
    // and return their registers to the free list
    while (!m_active.empty() && m_active.front().last_use < current_point) {
        u8 freed_reg = m_active.front().reg;
        m_free_regs.push_back(freed_reg);
        // Remove front by shifting (m_active is sorted, so we remove from front)
        for (u32 i = 1; i < m_active.size(); i++) {
            m_active[i - 1] = m_active[i];
        }
        m_active.pop_back();
    }
}

u16 BytecodeBuilder::add_constant(const BCConstant& c) {
    u16 index = static_cast<u16>(m_current_func->constants.size());
    m_current_func->constants.push_back(c);
    return index;
}

u16 BytecodeBuilder::add_int_constant(i64 value) {
    // Check if we can use immediate
    if (value >= IMM16_MIN && value <= IMM16_MAX) {
        return static_cast<u16>(value);  // Used directly as immediate
    }
    return add_constant(BCConstant::make_int(value));
}

u16 BytecodeBuilder::add_float_constant(f64 value) {
    return add_constant(BCConstant::make_float(value));
}

u16 BytecodeBuilder::add_string_constant(const char* data, u32 length) {
    return add_constant(BCConstant::make_string(data, length));
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

void BytecodeBuilder::lower_block(IRBlock* block) {
    // Already handled in build_function
}

void BytecodeBuilder::lower_instruction(IRInst* inst) {
    u8 dst = get_register(inst->result);

    switch (inst->op) {
        case IROp::ConstNull:
            emit_abi(Opcode::LOAD_NULL, dst, 0);
            break;

        case IROp::ConstBool:
            if (inst->const_data.bool_val) {
                emit_abi(Opcode::LOAD_TRUE, dst, 0);
            } else {
                emit_abi(Opcode::LOAD_FALSE, dst, 0);
            }
            break;

        case IROp::ConstInt: {
            i64 value = inst->const_data.int_val;
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
            f32 fval = inst->const_data.f32_val;
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
            u16 const_idx = add_constant(BCConstant::make_float(inst->const_data.f64_val));
            emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            break;
        }

        case IROp::ConstString: {
            StringView sv = inst->const_data.string_val;
            u16 const_idx = add_constant(BCConstant::make_string(sv.data(), sv.size()));
            emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            break;
        }

        // Binary operations
        case IROp::AddI:
        case IROp::SubI:
        case IROp::MulI:
        case IROp::DivI:
        case IROp::ModI:
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
        case IROp::EqI:
        case IROp::NeI:
        case IROp::LtI:
        case IROp::LeI:
        case IROp::GtI:
        case IROp::GeI:
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
            u8 left = get_register(inst->binary.left);
            u8 right = get_register(inst->binary.right);
            emit_abc(get_opcode(inst->op), dst, left, right);
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
            u8 src = get_register(inst->unary);
            emit_abc(get_opcode(inst->op), dst, src, 0);
            break;
        }

        case IROp::Copy: {
            u8 src = get_register(inst->unary);
            if (dst != src) {
                emit_abc(Opcode::MOV, dst, src, 0);
            }
            break;
        }

        case IROp::BlockArg:
            // Block arguments are handled by MOV instructions at jump sites
            // The value should already be in the register
            break;

        case IROp::Call: {
            StringView func_name = inst->call.func_name;
            auto it = m_func_indices.find(func_name);
            if (it == m_func_indices.end()) {
                // Internal compiler error: function should have been resolved by semantic analysis
                report_error("Internal error: function not found during bytecode lowering");
                break;
            }
            u8 func_idx = static_cast<u8>(it->second);
            u8 arg_count = static_cast<u8>(inst->call.args.size());

            // Get callee function to check for pointer parameters
            IRFunction* callee_func = m_ir_module->functions[func_idx];

            // Check if return type is a struct
            u32 ret_slot_count = get_struct_slot_count(inst->type);
            bool returns_small_struct = (ret_slot_count > 0 && ret_slot_count <= 4);
            u8 ret_reg_count = returns_small_struct ? static_cast<u8>((ret_slot_count + 1) / 2) : 1;

            // Copy arguments to consecutive registers starting from dst+ret_reg_count
            // (calling convention: arguments follow the destination/return registers)
            // Track cumulative register offset for multi-register struct arguments
            u8 first_arg_reg = dst + ret_reg_count;
            u8 arg_reg_offset = 0;
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                ValueId arg_val = inst->call.args[i];
                u8 arg_src = get_register(arg_val);

                // Check if this parameter is a pointer (out/inout)
                bool param_is_ptr = (i < callee_func->param_is_ptr.size() && callee_func->param_is_ptr[i]);

                if (param_is_ptr) {
                    // Pointer parameter: pass the pointer directly (already computed by gen_lvalue_addr)
                    if (arg_src != first_arg_reg + arg_reg_offset) {
                        emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                    }
                    arg_reg_offset += 1;
                } else {
                    // Check if argument is a struct
                    auto type_it = m_value_types.find(arg_val.id);
                    Type* arg_type = (type_it != m_value_types.end()) ? type_it->second : nullptr;
                    u32 arg_slot_count = get_struct_slot_count(arg_type);

                    if (arg_slot_count > 0 && arg_slot_count <= 4) {
                        // Small struct: load struct data from memory to consecutive registers
                        u8 arg_reg_count = static_cast<u8>((arg_slot_count + 1) / 2);
                        emit_abc(Opcode::STRUCT_LOAD_REGS, first_arg_reg + arg_reg_offset, arg_src, static_cast<u8>(arg_slot_count));
                        emit(0);  // Padding word
                        arg_reg_offset += arg_reg_count;
                    } else if (arg_slot_count > 4) {
                        // Large struct: pass pointer
                        if (arg_src != first_arg_reg + arg_reg_offset) {
                            emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                        }
                        arg_reg_offset += 1;
                    } else {
                        // Regular value
                        if (arg_src != first_arg_reg + arg_reg_offset) {
                            emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                        }
                        arg_reg_offset += 1;
                    }
                }
            }

            // Emit call: dst = call func_idx(args...)
            // Format: [CALL][dst][func_idx][arg_count]
            emit_abc(Opcode::CALL, dst, func_idx, arg_count);

            // For small struct returns, dst now contains packed struct data in consecutive registers
            // Allocate stack space and unpack
            if (returns_small_struct) {
                u32 stack_offset = m_next_stack_slot;
                m_next_stack_slot += ret_slot_count;
                m_value_to_stack_slot[inst->result.id] = stack_offset;

                // Get address of stack space
                u8 stack_ptr_reg = dst;  // Reuse dst as stack pointer since we're about to overwrite
                // Actually, we need a temp register. Let's use dst after storing the packed data.
                // The struct data is in dst, dst+1, so we need to:
                // 1. Get stack address into another register
                // 2. Store packed data to stack
                // But we're running out of registers... Let's allocate a temp.

                // For simplicity, use the first return register (dst) as the stack pointer
                // after we've unpacked. We need to:
                // 1. Store packed data from registers to stack
                // First, get the stack address
                u8 temp_reg = bump_register();  // Allocate temp register for stack address
                emit_abi(Opcode::STACK_ADDR, temp_reg, static_cast<u16>(stack_offset));
                emit_abc(Opcode::STRUCT_STORE_REGS, temp_reg, dst, static_cast<u8>(ret_slot_count));
                emit(0);  // Padding word
                // Now dst should point to the stack location
                emit_abc(Opcode::MOV, dst, temp_reg, 0);
            }
            break;
        }

        case IROp::CallNative: {
            // Similar to Call but uses CALL_NATIVE opcode
            u8 func_idx = inst->call.native_index;
            u8 arg_count = static_cast<u8>(inst->call.args.size());

            // Copy arguments to consecutive registers starting from dst+1
            u8 first_arg_reg = dst + 1;
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                u8 arg_src = get_register(inst->call.args[i]);
                if (arg_src != first_arg_reg + i) {
                    emit_abc(Opcode::MOV, first_arg_reg + i, arg_src, 0);
                }
            }

            // Emit call: dst = call_native func_idx(args...)
            emit_abc(Opcode::CALL_NATIVE, dst, func_idx, arg_count);
            break;
        }

        case IROp::CallExternal: {
            // Cross-module function call - resolved at link time via static linking
            // After linking, all functions are in the same module, so we emit a regular CALL
            StringView func_name = inst->call_external.func_name;
            u8 arg_count = static_cast<u8>(inst->call_external.args.size());

            // Look up function in the merged module's function table
            auto it = m_func_indices.find(func_name);
            if (it == m_func_indices.end()) {
                report_error("Internal error: external function not found during linking");
                break;
            }
            u8 func_idx = static_cast<u8>(it->second);

            // Get callee function to check for pointer parameters
            IRFunction* callee_func = m_ir_module->functions[func_idx];

            // Check if return type is a struct
            u32 ret_slot_count = get_struct_slot_count(inst->type);
            bool returns_small_struct = (ret_slot_count > 0 && ret_slot_count <= 4);
            u8 ret_reg_count = returns_small_struct ? static_cast<u8>((ret_slot_count + 1) / 2) : 1;

            // Copy arguments to consecutive registers starting from dst+ret_reg_count
            u8 first_arg_reg = dst + ret_reg_count;
            u8 arg_reg_offset = 0;
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                ValueId arg_val = inst->call_external.args[i];
                u8 arg_src = get_register(arg_val);

                // Check if this parameter is a pointer (out/inout)
                bool param_is_ptr = (i < callee_func->param_is_ptr.size() && callee_func->param_is_ptr[i]);

                if (param_is_ptr) {
                    // Pointer parameter: pass the pointer directly
                    if (arg_src != first_arg_reg + arg_reg_offset) {
                        emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                    }
                    arg_reg_offset += 1;
                } else {
                    // Check if argument is a struct
                    auto type_it = m_value_types.find(arg_val.id);
                    Type* arg_type = (type_it != m_value_types.end()) ? type_it->second : nullptr;
                    u32 arg_slot_count = get_struct_slot_count(arg_type);

                    if (arg_slot_count > 0 && arg_slot_count <= 4) {
                        // Small struct: load struct data from memory to consecutive registers
                        u8 arg_reg_count = static_cast<u8>((arg_slot_count + 1) / 2);
                        emit_abc(Opcode::STRUCT_LOAD_REGS, first_arg_reg + arg_reg_offset, arg_src, static_cast<u8>(arg_slot_count));
                        emit(0);  // Padding word
                        arg_reg_offset += arg_reg_count;
                    } else if (arg_slot_count > 4) {
                        // Large struct: pass pointer
                        if (arg_src != first_arg_reg + arg_reg_offset) {
                            emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                        }
                        arg_reg_offset += 1;
                    } else {
                        // Regular value
                        if (arg_src != first_arg_reg + arg_reg_offset) {
                            emit_abc(Opcode::MOV, first_arg_reg + arg_reg_offset, arg_src, 0);
                        }
                        arg_reg_offset += 1;
                    }
                }
            }

            // Emit regular CALL instruction (statically linked)
            emit_abc(Opcode::CALL, dst, func_idx, arg_count);

            // For small struct returns, handle unpacking
            if (returns_small_struct) {
                u32 stack_offset = m_next_stack_slot;
                m_next_stack_slot += ret_slot_count;
                m_value_to_stack_slot[inst->result.id] = stack_offset;

                u8 temp_reg = bump_register();
                emit_abi(Opcode::STACK_ADDR, temp_reg, static_cast<u16>(stack_offset));
                emit_abc(Opcode::STRUCT_STORE_REGS, temp_reg, dst, static_cast<u8>(ret_slot_count));
                emit(0);  // Padding word
                emit_abc(Opcode::MOV, dst, temp_reg, 0);
            }
            break;
        }

        case IROp::GetField: {
            // Format: [GET_FIELD dst obj slot_count] + [slot_offset:16 padding:16]
            u8 obj = get_register(inst->field.object);
            u8 slot_count = static_cast<u8>(inst->field.slot_count);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::GET_FIELD, dst, obj, slot_count);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            // f32 fields are loaded as their 32-bit pattern and stay that way
            // They'll be processed by f32-specific opcodes (ADD_F, etc.)
            break;
        }

        case IROp::GetFieldAddr: {
            // Format: [GET_FIELD_ADDR dst obj 0] + [slot_offset:16 padding:16]
            // Computes: dst = obj_ptr + slot_offset * 4 (pointer arithmetic)
            u8 obj = get_register(inst->field.object);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::GET_FIELD_ADDR, dst, obj, 0);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            break;
        }

        case IROp::SetField: {
            // Format: [SET_FIELD obj val slot_count] + [slot_offset:16 padding:16]
            u8 obj = get_register(inst->field.object);
            u8 val = get_register(inst->store_value);
            u8 slot_count = static_cast<u8>(inst->field.slot_count);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::SET_FIELD, obj, val, slot_count);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
            break;
        }

        case IROp::RefInc: {
            u8 ptr = get_register(inst->unary);
            emit_abc(Opcode::REF_INC, ptr, 0, 0);
            break;
        }

        case IROp::RefDec: {
            u8 ptr = get_register(inst->unary);
            emit_abc(Opcode::REF_DEC, ptr, 0, 0);
            break;
        }

        case IROp::WeakCheck: {
            u8 weak = get_register(inst->unary);
            emit_abc(Opcode::WEAK_CHECK, dst, weak, 0);
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
                m_module->types.push_back({type_name, size_bytes, struct_type->struct_info.slot_count});
                m_type_indices[type_name] = type_idx;
            }

            emit_abi(Opcode::NEW_OBJ, dst, type_idx);
            break;
        }

        case IROp::Delete: {
            u8 ptr_reg = get_register(inst->unary);
            // In constraint reference mode, interpreter will check ref_count == 0
            emit_abc(Opcode::DEL_OBJ, ptr_reg, 0, 0);
            break;
        }

        case IROp::StackAlloc: {
            // Allocate slots on the local stack
            u32 slot_count = inst->stack_alloc.slot_count;
            u32 slot_offset = m_next_stack_slot;
            m_next_stack_slot += slot_count;

            // Record the stack slot offset for this value
            m_value_to_stack_slot[inst->result.id] = slot_offset;

            // Emit STACK_ADDR to get a pointer to the allocated space
            emit_abi(Opcode::STACK_ADDR, dst, static_cast<u16>(slot_offset));
            break;
        }

        case IROp::StructCopy: {
            // Memory-to-memory struct copy
            u8 dest_ptr = get_register(inst->struct_copy.dest_ptr);
            u8 src_ptr = get_register(inst->struct_copy.source_ptr);
            u8 slot_count = static_cast<u8>(inst->struct_copy.slot_count);
            emit_abc(Opcode::STRUCT_COPY, dest_ptr, src_ptr, slot_count);
            break;
        }

        case IROp::LoadPtr: {
            // Load value through pointer - reuse GET_FIELD with offset 0
            u8 ptr_reg = get_register(inst->load_ptr.ptr);
            u8 slot_count = static_cast<u8>(inst->load_ptr.slot_count);
            emit_abc(Opcode::GET_FIELD, dst, ptr_reg, slot_count);
            emit(0);  // offset = 0
            break;
        }

        case IROp::StorePtr: {
            // Store value through pointer - reuse SET_FIELD with offset 0
            u8 ptr_reg = get_register(inst->store_ptr.ptr);
            u8 val_reg = get_register(inst->store_ptr.value);
            u8 slot_count = static_cast<u8>(inst->store_ptr.slot_count);
            emit_abc(Opcode::SET_FIELD, ptr_reg, val_reg, slot_count);
            emit(0);  // offset = 0
            break;
        }

        case IROp::VarAddr: {
            // Get address of local variable - lookup its stack slot and emit STACK_ADDR
            StringView name = inst->var_addr.name;
            // Look up the variable's value ID to find its stack slot
            // For now, we need to find if it has a corresponding StackAlloc
            // or if it's just a regular value.
            // Since variables passed as out/inout need to be stack-allocated,
            // we should track this. For now, let's allocate a new slot.

            // Check if this variable already has a stack slot from StackAlloc
            // If not, we need to allocate one on-demand
            auto it = m_var_name_to_stack_slot.find(name);
            if (it != m_var_name_to_stack_slot.end()) {
                emit_abi(Opcode::STACK_ADDR, dst, static_cast<u16>(it->second));
            } else {
                // Allocate a new stack slot for this variable
                u32 slot_offset = m_next_stack_slot;
                m_next_stack_slot += 1;  // Assume 1 slot for now (primitives)
                m_var_name_to_stack_slot[name] = slot_offset;
                emit_abi(Opcode::STACK_ADDR, dst, static_cast<u16>(slot_offset));
            }
            break;
        }

        case IROp::Cast: {
            u8 src = get_register(inst->cast.source);
            Type* source_type = inst->cast.source_type;
            Type* target_type = inst->type;

            emit_cast_bytecode(dst, src, source_type, target_type);
            break;
        }
    }
}

void BytecodeBuilder::emit_block_args(const JumpTarget& target) {
    if (!target.block.is_valid() || target.block.id >= m_current_ir_func->blocks.size()) return;
    IRBlock* target_block = m_current_ir_func->blocks[target.block.id];

    // Emit MOV instructions for each block argument
    for (u32 i = 0; i < target.args.size() && i < target_block->params.size(); i++) {
        u8 src = get_register(target.args[i].value);
        u8 dst = get_register(target_block->params[i].value);
        if (src != dst) {
            emit_abc(Opcode::MOV, dst, src, 0);
        }
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
            u8 cond = get_register(term.branch.condition);

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
            i16 skip_offset = static_cast<i16>(else_label - jmp_if_not_idx - 1);
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
                u8 ret = get_register(term.return_value);

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

void BytecodeBuilder::patch_jumps() {
    for (const JumpPatch& patch : m_jump_patches) {
        auto it = m_block_offsets.find(patch.target_block.id);
        if (it == m_block_offsets.end()) {
            continue;  // Invalid target
        }

        u32 target_offset = it->second;
        u32 current_offset = patch.instruction_index;
        i16 relative_offset = static_cast<i16>(target_offset - current_offset - 1);

        // Patch the instruction
        u32& instr = m_current_func->code[patch.instruction_index];
        Opcode op = decode_opcode(instr);
        u8 a = decode_a(instr);

        instr = encode_aoff(op, a, relative_offset);
    }
}

bool BytecodeBuilder::is_large_struct(Type* type) const {
    if (!type || !type->is_struct()) return false;
    return type->struct_info.slot_count > 4;
}

u32 BytecodeBuilder::get_struct_slot_count(Type* type) const {
    if (!type || !type->is_struct()) return 0;
    return type->struct_info.slot_count;
}

Opcode BytecodeBuilder::get_opcode(IROp op) const {
    switch (op) {
        // Integer arithmetic
        case IROp::AddI:    return Opcode::ADD_I;
        case IROp::SubI:    return Opcode::SUB_I;
        case IROp::MulI:    return Opcode::MUL_I;
        case IROp::DivI:    return Opcode::DIV_I;
        case IROp::ModI:    return Opcode::MOD_I;
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

        // Logical
        case IROp::Not:     return Opcode::NOT;
        case IROp::And:     return Opcode::AND;
        case IROp::Or:      return Opcode::OR;

        // Bitwise
        case IROp::BitAnd:  return Opcode::BIT_AND;
        case IROp::BitOr:   return Opcode::BIT_OR;
        case IROp::BitXor:  return Opcode::BIT_XOR;
        case IROp::BitNot:  return Opcode::BIT_NOT;
        case IROp::Shl:     return Opcode::SHL;
        case IROp::Shr:     return Opcode::SHR;

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

}

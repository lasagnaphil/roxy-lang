#include "roxy/compiler/lowering.hpp"
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
    m_next_reg = 0;
    m_next_stack_slot = 0;

    // Allocate registers for function parameters and record types
    for (const auto& param : ir_func->params) {
        allocate_register(param.value);
        if (param.type) {
            m_value_types[param.value.id] = param.type;
        }
    }

    // First pass: allocate registers for all values and record block offsets
    for (IRBlock* block : ir_func->blocks) {
        // Allocate registers for block parameters and record types
        for (const auto& param : block->params) {
            if (!has_register(param.value)) {
                allocate_register(param.value);
            }
            if (param.type) {
                m_value_types[param.value.id] = param.type;
            }
        }

        // Allocate registers for instruction results and record types
        for (IRInst* inst : block->instructions) {
            if (inst->result.is_valid() && !has_register(inst->result)) {
                allocate_register(inst->result);
            }
            if (inst->result.is_valid() && inst->type) {
                m_value_types[inst->result.id] = inst->type;
            }

            // For calls, we also need registers for arguments (at dst+1, dst+2, ...)
            // For struct returns, we need extra registers for the packed struct data
            if ((inst->op == IROp::Call || inst->op == IROp::CallNative) && inst->result.is_valid()) {
                u8 dst = get_register(inst->result);
                u32 extra_regs_for_return = 0;
                u32 ret_slot_count = get_struct_slot_count(inst->type);
                if (ret_slot_count > 0 && ret_slot_count <= 4) {
                    // Small struct return needs (slot_count + 1) / 2 registers
                    extra_regs_for_return = (ret_slot_count + 1) / 2;
                }

                u8 needed_regs = dst + static_cast<u8>(extra_regs_for_return) + 1 + static_cast<u8>(inst->call.args.size());
                while (m_next_reg < needed_regs) {
                    m_next_reg++;
                }
            }
        }
    }

    // Emit function prologue: unpack struct parameters from registers to local stack
    for (u32 i = 0; i < ir_func->params.size(); i++) {
        const auto& param = ir_func->params[i];

        // Check if this parameter is a pointer (out/inout parameter)
        bool is_ptr_param = (i < ir_func->param_is_ptr.size() && ir_func->param_is_ptr[i]);

        // Skip unpacking for pointer parameters - they already contain a pointer
        if (is_ptr_param) {
            continue;
        }

        u32 slot_count = get_struct_slot_count(param.type);

        if (slot_count > 0 && slot_count <= 4) {
            // Small struct: param register holds packed data
            // Allocate local stack space and unpack
            u32 stack_offset = m_next_stack_slot;
            m_next_stack_slot += slot_count;
            m_value_to_stack_slot[param.value.id] = stack_offset;

            u8 param_reg = get_register(param.value);

            // Get stack address into a new register
            u8 stack_ptr_reg = m_next_reg++;
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

            u8 param_reg = get_register(param.value);  // Source pointer (caller's data)

            // Get local stack address into a new register
            u8 stack_ptr_reg = m_next_reg++;
            emit_abi(Opcode::STACK_ADDR, stack_ptr_reg, static_cast<u16>(stack_offset));

            // Copy struct data from caller to local stack (value semantics)
            emit_abc(Opcode::STRUCT_COPY, stack_ptr_reg, param_reg, static_cast<u8>(slot_count));

            // Remap the parameter value to the local stack pointer register
            m_value_to_reg[param.value.id] = stack_ptr_reg;
        }
    }

    // Second pass: emit bytecode
    for (IRBlock* block : ir_func->blocks) {
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

u8 BytecodeBuilder::allocate_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    auto it = m_value_to_reg.find(value.id);
    if (it != m_value_to_reg.end()) {
        return it->second;
    }

    assert(m_next_reg < 255 && "Register overflow");
    u8 reg = m_next_reg++;
    m_value_to_reg[value.id] = reg;
    return reg;
}

u8 BytecodeBuilder::get_register(ValueId value) {
    if (!value.is_valid()) return 0xFF;

    auto it = m_value_to_reg.find(value.id);
    assert(it != m_value_to_reg.end() && "Value not allocated");
    return it->second;
}

bool BytecodeBuilder::has_register(ValueId value) const {
    if (!value.is_valid()) return false;
    return m_value_to_reg.find(value.id) != m_value_to_reg.end();
}

u16 BytecodeBuilder::add_constant(const BCConstant& c) {
    u16 index = static_cast<u16>(m_current_func->constants.size());
    m_current_func->constants.push_back(c);
    return index;
}

u16 BytecodeBuilder::add_int_constant(i64 value) {
    // Check if we can use immediate
    if (value >= -32768 && value <= 32767) {
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
            if (value >= -32768 && value <= 32767) {
                emit_abi(Opcode::LOAD_INT, dst, static_cast<u16>(static_cast<i16>(value)));
            } else {
                u16 const_idx = add_constant(BCConstant::make_int(value));
                emit_abi(Opcode::LOAD_CONST, dst, const_idx);
            }
            break;
        }

        case IROp::ConstFloat: {
            u16 const_idx = add_constant(BCConstant::make_float(inst->const_data.float_val));
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
        case IROp::BitAnd:
        case IROp::BitOr:
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
        case IROp::BitNot:
        case IROp::Not:
        case IROp::I2F:
        case IROp::F2I:
        case IROp::I2B:
        case IROp::B2I: {
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
            u8 first_arg_reg = dst + ret_reg_count;
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                ValueId arg_val = inst->call.args[i];
                u8 arg_src = get_register(arg_val);

                // Check if this parameter is a pointer (out/inout)
                bool param_is_ptr = (i < callee_func->param_is_ptr.size() && callee_func->param_is_ptr[i]);

                if (param_is_ptr) {
                    // Pointer parameter: pass the pointer directly (already computed by gen_lvalue_addr)
                    if (arg_src != first_arg_reg + i) {
                        emit_abc(Opcode::MOV, first_arg_reg + static_cast<u8>(i), arg_src, 0);
                    }
                } else {
                    // Check if argument is a struct
                    auto type_it = m_value_types.find(arg_val.id);
                    Type* arg_type = (type_it != m_value_types.end()) ? type_it->second : nullptr;
                    u32 arg_slot_count = get_struct_slot_count(arg_type);

                    if (arg_slot_count > 0 && arg_slot_count <= 4) {
                        // Small struct: load struct data from memory to consecutive registers
                        u8 arg_reg_count = static_cast<u8>((arg_slot_count + 1) / 2);
                        emit_abc(Opcode::STRUCT_LOAD_REGS, first_arg_reg + static_cast<u8>(i), arg_src, static_cast<u8>(arg_slot_count));
                        emit(0);  // Padding word
                        // TODO: Track that this argument uses multiple registers
                    } else if (arg_slot_count > 4) {
                        // Large struct: pass pointer
                        if (arg_src != first_arg_reg + i) {
                            emit_abc(Opcode::MOV, first_arg_reg + static_cast<u8>(i), arg_src, 0);
                        }
                    } else {
                        // Regular value
                        if (arg_src != first_arg_reg + i) {
                            emit_abc(Opcode::MOV, first_arg_reg + static_cast<u8>(i), arg_src, 0);
                        }
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
                u8 temp_reg = m_next_reg++;  // Allocate temp register for stack address
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
            // Cross-module function call
            StringView module_name = inst->call_external.module_name;
            StringView func_name = inst->call_external.func_name;
            u8 arg_count = static_cast<u8>(inst->call_external.args.size());

            // Find or add external function reference
            u8 ext_func_idx = 0;
            bool found = false;
            for (u32 i = 0; i < m_module->external_functions.size(); i++) {
                if (m_module->external_functions[i].module_name == module_name &&
                    m_module->external_functions[i].func_name == func_name) {
                    ext_func_idx = static_cast<u8>(i);
                    found = true;
                    break;
                }
            }
            if (!found) {
                ext_func_idx = static_cast<u8>(m_module->external_functions.size());
                BCExternalFunction ext_func;
                ext_func.module_name = module_name;
                ext_func.func_name = func_name;
                m_module->external_functions.push_back(ext_func);
            }

            // Copy arguments to consecutive registers starting from dst+1
            u8 first_arg_reg = dst + 1;
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                u8 arg_src = get_register(inst->call_external.args[i]);
                if (arg_src != first_arg_reg + i) {
                    emit_abc(Opcode::MOV, first_arg_reg + static_cast<u8>(i), arg_src, 0);
                }
            }

            // Emit call: dst = call_external ext_func_idx(args...)
            emit_abc(Opcode::CALL_EXTERNAL, dst, ext_func_idx, arg_count);
            break;
        }

        case IROp::GetField: {
            // Format: [GET_FIELD dst obj slot_count] + [slot_offset:16 padding:16]
            u8 obj = get_register(inst->field.object);
            u8 slot_count = static_cast<u8>(inst->field.slot_count);
            u16 slot_offset = static_cast<u16>(inst->field.slot_offset);
            emit_abc(Opcode::GET_FIELD, dst, obj, slot_count);
            emit(static_cast<u32>(slot_offset));  // Second instruction word with slot offset
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

        case IROp::GetIndex: {
            u8 obj = get_register(inst->index.object);
            u8 idx = get_register(inst->index.index);
            emit_abc(Opcode::GET_INDEX, dst, obj, idx);
            break;
        }

        case IROp::SetIndex: {
            u8 obj = get_register(inst->index.object);
            u8 idx = get_register(inst->index.index);
            u8 val = get_register(inst->store_value);
            emit_abc(Opcode::SET_INDEX, obj, idx, val);
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
    }
}

void BytecodeBuilder::emit_block_args(const JumpTarget& target) {
    // Get target block
    IRBlock* target_block = nullptr;
    for (IRBlock* block : m_current_ir_func->blocks) {
        if (block->id == target.block) {
            target_block = block;
            break;
        }
    }

    if (target_block == nullptr) return;

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

            // Emit then-branch arguments
            emit_block_args(term.branch.then_target);

            // Record conditional jump for patching
            JumpPatch then_patch;
            then_patch.instruction_index = m_current_func->code.size();
            then_patch.target_block = term.branch.then_target.block;
            m_jump_patches.push_back(then_patch);

            // Emit conditional jump with placeholder offset
            emit_aoff(Opcode::JMP_IF, cond, 0);

            // Emit else-branch arguments
            emit_block_args(term.branch.else_target);

            // Record unconditional jump for patching
            JumpPatch else_patch;
            else_patch.instruction_index = m_current_func->code.size();
            else_patch.target_block = term.branch.else_target.block;
            m_jump_patches.push_back(else_patch);

            // Emit unconditional jump with placeholder offset
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
            emit_abc(Opcode::HALT, 0, 0, 0);
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

        // Float arithmetic
        case IROp::AddF:    return Opcode::ADD_F;
        case IROp::SubF:    return Opcode::SUB_F;
        case IROp::MulF:    return Opcode::MUL_F;
        case IROp::DivF:    return Opcode::DIV_F;
        case IROp::NegF:    return Opcode::NEG_F;

        // Integer comparisons
        case IROp::EqI:     return Opcode::EQ_I;
        case IROp::NeI:     return Opcode::NE_I;
        case IROp::LtI:     return Opcode::LT_I;
        case IROp::LeI:     return Opcode::LE_I;
        case IROp::GtI:     return Opcode::GT_I;
        case IROp::GeI:     return Opcode::GE_I;

        // Float comparisons
        case IROp::EqF:     return Opcode::EQ_F;
        case IROp::NeF:     return Opcode::NE_F;
        case IROp::LtF:     return Opcode::LT_F;
        case IROp::LeF:     return Opcode::LE_F;
        case IROp::GtF:     return Opcode::GT_F;
        case IROp::GeF:     return Opcode::GE_F;

        // Logical
        case IROp::Not:     return Opcode::NOT;
        case IROp::And:     return Opcode::AND;
        case IROp::Or:      return Opcode::OR;

        // Bitwise
        case IROp::BitAnd:  return Opcode::BIT_AND;
        case IROp::BitOr:   return Opcode::BIT_OR;
        case IROp::BitNot:  return Opcode::BIT_NOT;

        // Type conversions
        case IROp::I2F:     return Opcode::I2F;
        case IROp::F2I:     return Opcode::F2I;
        case IROp::I2B:     return Opcode::I2B;
        case IROp::B2I:     return Opcode::B2I;

        default:
            return Opcode::NOP;
    }
}

}

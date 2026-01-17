#include "roxy/compiler/lowering.hpp"

#include <cassert>

namespace rx {

BytecodeBuilder::BytecodeBuilder()
    : m_current_func(nullptr)
    , m_current_ir_func(nullptr)
    , m_next_reg(0)
    , m_module(nullptr)
    , m_ir_module(nullptr)
{}

BCModule* BytecodeBuilder::build(IRModule* ir_module) {
    m_ir_module = ir_module;
    m_module = new BCModule();
    m_module->name = ir_module->name;

    // Build function name index map
    m_func_indices.clear();
    for (u32 i = 0; i < ir_module->functions.size(); i++) {
        m_func_indices[ir_module->functions[i]->name] = i;
    }

    // Build each function
    for (IRFunction* ir_func : ir_module->functions) {
        BCFunction* bc_func = build_function(ir_func);
        m_module->functions.push_back(bc_func);
    }

    return m_module;
}

BCFunction* BytecodeBuilder::build_function(IRFunction* ir_func) {
    m_current_ir_func = ir_func;
    m_current_func = new BCFunction();
    m_current_func->name = ir_func->name;
    m_current_func->param_count = ir_func->params.size();

    // Reset state
    m_value_to_reg.clear();
    m_block_offsets.clear();
    m_jump_patches.clear();
    m_next_reg = 0;

    // Allocate registers for function parameters
    for (const auto& param : ir_func->params) {
        allocate_register(param.value);
    }

    // First pass: allocate registers for all values and record block offsets
    for (IRBlock* block : ir_func->blocks) {
        // Allocate registers for block parameters
        for (const auto& param : block->params) {
            if (!has_register(param.value)) {
                allocate_register(param.value);
            }
        }

        // Allocate registers for instruction results
        for (IRInst* inst : block->instructions) {
            if (inst->result.is_valid() && !has_register(inst->result)) {
                allocate_register(inst->result);
            }

            // For calls, we also need registers for arguments (at dst+1, dst+2, ...)
            if (inst->op == IROp::Call && inst->result.is_valid()) {
                u8 dst = get_register(inst->result);
                u8 needed_regs = dst + 1 + static_cast<u8>(inst->call.args.size());
                while (m_next_reg < needed_regs) {
                    m_next_reg++;
                }
            }
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
                // Could be a native function - check module
                // For now, emit as regular call and let runtime handle it
                assert(false && "Function not found");
            }
            u8 func_idx = static_cast<u8>(it->second);
            u8 arg_count = static_cast<u8>(inst->call.args.size());

            // Copy arguments to consecutive registers starting from dst+1
            // (calling convention: arguments follow the destination register)
            u8 first_arg_reg = dst + 1;
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                u8 arg_src = get_register(inst->call.args[i]);
                if (arg_src != first_arg_reg + i) {
                    emit_abc(Opcode::MOV, first_arg_reg + i, arg_src, 0);
                }
            }

            // Emit call: dst = call func_idx(args...)
            // Format: [CALL][dst][func_idx][arg_count]
            emit_abc(Opcode::CALL, dst, func_idx, arg_count);
            break;
        }

        case IROp::CallNative: {
            // Similar to Call but uses CALL_NATIVE opcode
            // TODO: Implement native function call lowering
            break;
        }

        case IROp::GetField: {
            u8 obj = get_register(inst->field.object);
            u16 field_idx = static_cast<u16>(inst->field.field_index);
            emit_abc(Opcode::GET_FIELD, dst, obj, 0);
            // Need to encode field index - use next instruction or embed differently
            // For now, use format B encoding
            m_current_func->code.pop_back();
            emit((static_cast<u32>(Opcode::GET_FIELD) << 24) |
                 (static_cast<u32>(dst) << 16) |
                 (static_cast<u32>(obj) << 8) |
                 (field_idx & 0xFF));
            break;
        }

        case IROp::SetField: {
            u8 obj = get_register(inst->field.object);
            u8 val = get_register(inst->store_value);
            u16 field_idx = static_cast<u16>(inst->field.field_index);
            emit((static_cast<u32>(Opcode::SET_FIELD) << 24) |
                 (static_cast<u32>(obj) << 16) |
                 (static_cast<u32>(val) << 8) |
                 (field_idx & 0xFF));
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
            // TODO: Type lookup and encoding
            u16 type_idx = 0;  // Placeholder
            emit_abi(Opcode::NEW_OBJ, dst, type_idx);
            break;
        }

        case IROp::Delete: {
            u8 ptr = get_register(inst->unary);
            emit_abc(Opcode::DEL_OBJ, ptr, 0, 0);
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
            if (term.return_value.is_valid()) {
                u8 ret = get_register(term.return_value);
                emit_abc(Opcode::RET, ret, 0, 0);
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

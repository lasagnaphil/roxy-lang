#include "roxy/vm/bytecode.hpp"
#include "roxy/core/fmt/format.h"

namespace rx {

const char* opcode_to_string(Opcode op) {
    switch (op) {
        // Constants and Moves
        case Opcode::LOAD_NULL:     return "LOAD_NULL";
        case Opcode::LOAD_TRUE:     return "LOAD_TRUE";
        case Opcode::LOAD_FALSE:    return "LOAD_FALSE";
        case Opcode::LOAD_INT:      return "LOAD_INT";
        case Opcode::LOAD_CONST:    return "LOAD_CONST";
        case Opcode::MOV:           return "MOV";

        // Integer Arithmetic
        case Opcode::ADD_I:         return "ADD_I";
        case Opcode::SUB_I:         return "SUB_I";
        case Opcode::MUL_I:         return "MUL_I";
        case Opcode::DIV_I:         return "DIV_I";
        case Opcode::MOD_I:         return "MOD_I";
        case Opcode::NEG_I:         return "NEG_I";

        // Float Arithmetic
        case Opcode::ADD_F:         return "ADD_F";
        case Opcode::SUB_F:         return "SUB_F";
        case Opcode::MUL_F:         return "MUL_F";
        case Opcode::DIV_F:         return "DIV_F";
        case Opcode::NEG_F:         return "NEG_F";

        // Bitwise Operations
        case Opcode::BIT_AND:       return "BIT_AND";
        case Opcode::BIT_OR:        return "BIT_OR";
        case Opcode::BIT_XOR:       return "BIT_XOR";
        case Opcode::BIT_NOT:       return "BIT_NOT";
        case Opcode::SHL:           return "SHL";
        case Opcode::SHR:           return "SHR";
        case Opcode::USHR:          return "USHR";

        // Integer Comparisons
        case Opcode::EQ_I:          return "EQ_I";
        case Opcode::NE_I:          return "NE_I";
        case Opcode::LT_I:          return "LT_I";
        case Opcode::LE_I:          return "LE_I";
        case Opcode::GT_I:          return "GT_I";
        case Opcode::GE_I:          return "GE_I";
        case Opcode::LT_U:          return "LT_U";
        case Opcode::LE_U:          return "LE_U";
        case Opcode::GT_U:          return "GT_U";
        case Opcode::GE_U:          return "GE_U";

        // Float Comparisons
        case Opcode::EQ_F:          return "EQ_F";
        case Opcode::NE_F:          return "NE_F";
        case Opcode::LT_F:          return "LT_F";
        case Opcode::LE_F:          return "LE_F";
        case Opcode::GT_F:          return "GT_F";
        case Opcode::GE_F:          return "GE_F";

        // Logical Operations
        case Opcode::NOT:           return "NOT";
        case Opcode::AND:           return "AND";
        case Opcode::OR:            return "OR";

        // Type Conversions
        case Opcode::I_TO_F64:      return "I_TO_F64";
        case Opcode::F64_TO_I:      return "F64_TO_I";
        case Opcode::I_TO_B:        return "I_TO_B";
        case Opcode::B_TO_I:        return "B_TO_I";
        case Opcode::TRUNC_S:       return "TRUNC_S";
        case Opcode::TRUNC_U:       return "TRUNC_U";
        case Opcode::F32_TO_F64:    return "F32_TO_F64";
        case Opcode::F64_TO_F32:    return "F64_TO_F32";
        case Opcode::I_TO_F32:      return "I_TO_F32";
        case Opcode::F32_TO_I:      return "F32_TO_I";

        // Control Flow
        case Opcode::JMP:           return "JMP";
        case Opcode::JMP_IF:        return "JMP_IF";
        case Opcode::JMP_IF_NOT:    return "JMP_IF_NOT";
        case Opcode::RET:           return "RET";
        case Opcode::RET_VOID:      return "RET_VOID";

        // Function Calls
        case Opcode::CALL:          return "CALL";
        case Opcode::CALL_NATIVE:   return "CALL_NATIVE";
        case Opcode::CALL_EXTERNAL: return "CALL_EXTERNAL";

        // Field Access
        case Opcode::GET_FIELD:     return "GET_FIELD";
        case Opcode::SET_FIELD:     return "SET_FIELD";
        case Opcode::STACK_ADDR:    return "STACK_ADDR";
        case Opcode::GET_FIELD_ADDR: return "GET_FIELD_ADDR";
        case Opcode::STRUCT_LOAD_REGS:  return "STRUCT_LOAD_REGS";
        case Opcode::STRUCT_STORE_REGS: return "STRUCT_STORE_REGS";
        case Opcode::STRUCT_COPY:       return "STRUCT_COPY";
        case Opcode::RET_STRUCT_SMALL:  return "RET_STRUCT_SMALL";

        // Index Access
        case Opcode::GET_INDEX:     return "GET_INDEX";
        case Opcode::SET_INDEX:     return "SET_INDEX";

        // Object Lifecycle
        case Opcode::NEW_OBJ:       return "NEW_OBJ";
        case Opcode::DEL_OBJ:       return "DEL_OBJ";

        // Reference Counting
        case Opcode::REF_INC:       return "REF_INC";
        case Opcode::REF_DEC:       return "REF_DEC";
        case Opcode::WEAK_CHECK:    return "WEAK_CHECK";

        // Debug
        case Opcode::NOP:           return "NOP";
        case Opcode::HALT:          return "HALT";

        default:                    return "UNKNOWN";
    }
}

void disassemble_instruction(u32 instr, u32 offset, Vector<char>& out) {
    Opcode op = decode_opcode(instr);
    u8 a = decode_a(instr);
    u8 b = decode_b(instr);
    u8 c = decode_c(instr);
    u16 imm = decode_imm16(instr);
    i16 soff = decode_offset(instr);

    auto append = [&out](const char* str) {
        while (*str) out.push_back(*str++);
    };

    // Print offset
    char buf[64];
    snprintf(buf, sizeof(buf), "%04u: %-12s ", offset, opcode_to_string(op));
    append(buf);

    switch (op) {
        // Format: dst
        case Opcode::LOAD_NULL:
        case Opcode::LOAD_TRUE:
        case Opcode::LOAD_FALSE:
        case Opcode::RET_VOID:
            snprintf(buf, sizeof(buf), "R%u", a);
            break;

        // Format: dst, imm16
        case Opcode::LOAD_INT:
        case Opcode::LOAD_CONST:
            snprintf(buf, sizeof(buf), "R%u, %d", a, static_cast<i16>(imm));
            break;

        // Format: dst, src
        case Opcode::MOV:
        case Opcode::NEG_I:
        case Opcode::NEG_F:
        case Opcode::BIT_NOT:
        case Opcode::NOT:
        case Opcode::I_TO_F64:
        case Opcode::F64_TO_I:
        case Opcode::I_TO_B:
        case Opcode::B_TO_I:
        case Opcode::F32_TO_F64:
        case Opcode::F64_TO_F32:
        case Opcode::I_TO_F32:
        case Opcode::F32_TO_I:
        case Opcode::REF_INC:
        case Opcode::REF_DEC:
        case Opcode::WEAK_CHECK:
        case Opcode::DEL_OBJ:
            snprintf(buf, sizeof(buf), "R%u, R%u", a, b);
            break;

        // Format: dst, src, bits
        case Opcode::TRUNC_S:
        case Opcode::TRUNC_U:
            snprintf(buf, sizeof(buf), "R%u, R%u, bits=%u", a, b, c);
            break;

        // Format: dst, src1, src2
        case Opcode::ADD_I:
        case Opcode::SUB_I:
        case Opcode::MUL_I:
        case Opcode::DIV_I:
        case Opcode::MOD_I:
        case Opcode::ADD_F:
        case Opcode::SUB_F:
        case Opcode::MUL_F:
        case Opcode::DIV_F:
        case Opcode::BIT_AND:
        case Opcode::BIT_OR:
        case Opcode::BIT_XOR:
        case Opcode::SHL:
        case Opcode::SHR:
        case Opcode::USHR:
        case Opcode::EQ_I:
        case Opcode::NE_I:
        case Opcode::LT_I:
        case Opcode::LE_I:
        case Opcode::GT_I:
        case Opcode::GE_I:
        case Opcode::LT_U:
        case Opcode::LE_U:
        case Opcode::GT_U:
        case Opcode::GE_U:
        case Opcode::EQ_F:
        case Opcode::NE_F:
        case Opcode::LT_F:
        case Opcode::LE_F:
        case Opcode::GT_F:
        case Opcode::GE_F:
        case Opcode::AND:
        case Opcode::OR:
        case Opcode::GET_INDEX:
        case Opcode::SET_INDEX:
            snprintf(buf, sizeof(buf), "R%u, R%u, R%u", a, b, c);
            break;

        // Format: offset
        case Opcode::JMP:
            snprintf(buf, sizeof(buf), "%+d -> %u", soff, offset + 1 + soff);
            break;

        // Format: reg, offset
        case Opcode::JMP_IF:
        case Opcode::JMP_IF_NOT:
            snprintf(buf, sizeof(buf), "R%u, %+d -> %u", a, soff, offset + 1 + soff);
            break;

        // Format: reg
        case Opcode::RET:
            snprintf(buf, sizeof(buf), "R%u", a);
            break;

        // Format: dst, func_idx, arg_count (args at dst+1, dst+2, ...)
        case Opcode::CALL:
        case Opcode::CALL_NATIVE:
        case Opcode::CALL_EXTERNAL:
            snprintf(buf, sizeof(buf), "R%u, func[%u], %u args from R%u", a, b, c, a + 1);
            break;

        // Format: dst, src, field_idx
        case Opcode::GET_FIELD:
        case Opcode::SET_FIELD:
            snprintf(buf, sizeof(buf), "R%u, R%u, field[%u]", a, b, imm);
            break;

        // Format: dst, type_idx
        case Opcode::NEW_OBJ:
            snprintf(buf, sizeof(buf), "R%u, type[%u]", a, imm);
            break;

        // Format: dst, imm16 (stack address)
        case Opcode::STACK_ADDR:
            snprintf(buf, sizeof(buf), "R%u, stack[%u]", a, imm);
            break;

        // Format: dst, src, slot_offset (two-word instruction)
        case Opcode::GET_FIELD_ADDR:
            snprintf(buf, sizeof(buf), "R%u, R%u  ; (two-word)", a, b);
            break;

        // Format: dst, src, slot_count (struct operations)
        case Opcode::STRUCT_LOAD_REGS:
            snprintf(buf, sizeof(buf), "R%u, R%u, slots=%u", a, b, c);
            break;

        case Opcode::STRUCT_STORE_REGS:
            snprintf(buf, sizeof(buf), "R%u, R%u, slots=%u", a, b, c);
            break;

        case Opcode::STRUCT_COPY:
            snprintf(buf, sizeof(buf), "R%u, R%u, slots=%u", a, b, c);
            break;

        case Opcode::RET_STRUCT_SMALL:
            snprintf(buf, sizeof(buf), "R%u, slots=%u", a, b);
            break;

        case Opcode::NOP:
        case Opcode::HALT:
            buf[0] = '\0';
            break;

        default:
            snprintf(buf, sizeof(buf), "0x%08X", instr);
            break;
    }

    append(buf);
    out.push_back('\n');
}

void disassemble_function(const BCFunction* func, Vector<char>& out) {
    auto append = [&out](const char* str) {
        while (*str) out.push_back(*str++);
    };

    char buf[128];
    snprintf(buf, sizeof(buf), "function %.*s (params: %u, regs: %u)\n",
             func->name.size(), func->name.data(),
             func->param_count, func->register_count);
    append(buf);

    // Constants
    if (!func->constants.empty()) {
        append("  constants:\n");
        for (u32 i = 0; i < func->constants.size(); i++) {
            const auto& c = func->constants[i];
            switch (c.type) {
                case BCConstant::Null:
                    snprintf(buf, sizeof(buf), "    [%u] null\n", i);
                    break;
                case BCConstant::Bool:
                    snprintf(buf, sizeof(buf), "    [%u] %s\n", i, c.as_bool ? "true" : "false");
                    break;
                case BCConstant::Int:
                    snprintf(buf, sizeof(buf), "    [%u] %lld\n", i, static_cast<long long>(c.as_int));
                    break;
                case BCConstant::Float:
                    snprintf(buf, sizeof(buf), "    [%u] %g\n", i, c.as_float);
                    break;
                case BCConstant::String:
                    snprintf(buf, sizeof(buf), "    [%u] \"%.*s\"\n", i,
                             c.as_string.length, c.as_string.data);
                    break;
            }
            append(buf);
        }
    }

    // Code
    append("  code:\n");
    for (u32 i = 0; i < func->code.size(); i++) {
        append("    ");
        disassemble_instruction(func->code[i], i, out);
    }
}

void disassemble_module(const BCModule* module, Vector<char>& out) {
    auto append = [&out](const char* str) {
        while (*str) out.push_back(*str++);
    };

    char buf[128];
    snprintf(buf, sizeof(buf), "module %.*s\n\n",
             module->name.size(), module->name.data());
    append(buf);

    // Native functions
    if (!module->native_functions.empty()) {
        append("native functions:\n");
        for (u32 i = 0; i < module->native_functions.size(); i++) {
            const auto& nf = module->native_functions[i];
            snprintf(buf, sizeof(buf), "  [%u] %.*s (params: %u)\n", i,
                     nf.name.size(), nf.name.data(), nf.param_count);
            append(buf);
        }
        append("\n");
    }

    // Functions
    for (u32 i = 0; i < module->functions.size(); i++) {
        snprintf(buf, sizeof(buf), "[%u] ", i);
        append(buf);
        disassemble_function(module->functions[i].get(), out);
        append("\n");
    }

    out.push_back('\0');
}

}

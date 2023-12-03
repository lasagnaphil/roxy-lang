#include "roxy/chunk.hpp"

#include <opcode.hpp>

#include "roxy/core/binary_search.hpp"
#include "roxy/fmt/core.h"

namespace rx {

void Chunk::write(u8 byte, u32 line) {
    if (m_lines.empty()) {
        m_lines.push_back(line);
        m_line_bytecode_starts.push_back(0);
    }

    if (line != -1 && m_lines.back() != line) {
        m_lines.push_back(line);
        m_line_bytecode_starts.push_back(m_bytecode.size());
    }

    m_bytecode.push_back(byte);
}

u32 Chunk::add_string(std::string_view str) {
    return m_constant_table.add_string(str);
}

u32 Chunk::add_constant(UntypedValue value) {
    return m_constant_table.add_value(value);
}

u32 Chunk::get_line(u32 bytecode_offset) {
    i32 i = binary_search(m_line_bytecode_starts.data(), m_line_bytecode_starts.size(), bytecode_offset);
    return m_lines[i];
}

void Chunk::print_disassembly() {
    fmt::print("== {} ==\n", m_name);
    for (i32 offset = 0; offset < m_bytecode.size(); ) {
        offset = disassemble_instruction(offset);
    }
}

u32 Chunk::disassemble_instruction(u32 offset) {
    fmt::print("{:04d} ", offset);
    u32 cur_line = get_line(offset);
    u32 prev_line = get_line(offset - 1);
    if (offset > 0 && cur_line == prev_line) {
        fmt::print("   | ");
    }
    else {
        fmt::print("{:4d} ", cur_line);
    }
    OpCode opcode = (OpCode)m_bytecode[offset];
    switch (opcode) {
    case OpCode::Nop:
    case OpCode::Break:
    case OpCode::Ld_Arg_0:
    case OpCode::Ld_Arg_1:
    case OpCode::Ld_Arg_2:
    case OpCode::Ld_Arg_3:
    case OpCode::Ld_Loc_0:
    case OpCode::Ld_Loc_1:
    case OpCode::Ld_Loc_2:
    case OpCode::Ld_Loc_3:
    case OpCode::St_Loc_0:
    case OpCode::St_Loc_1:
    case OpCode::St_Loc_2:
    case OpCode::St_Loc_3:
    case OpCode::Ld_Arg_i8_0:
    case OpCode::Ld_Arg_i8_1:
    case OpCode::Ld_Arg_i8_2:
    case OpCode::Ld_Arg_i8_3:
    case OpCode::Ld_Loc_i8_0:
    case OpCode::Ld_Loc_i8_1:
    case OpCode::Ld_Loc_i8_2:
    case OpCode::Ld_Loc_i8_3:
    case OpCode::St_Loc_i8_0:
    case OpCode::St_Loc_i8_1:
    case OpCode::St_Loc_i8_2:
    case OpCode::St_Loc_i8_3:
    case OpCode::LdC_Null:
    case OpCode::LdC_i4_M1:
    case OpCode::LdC_i4_0:
    case OpCode::LdC_i4_1:
    case OpCode::LdC_i4_2:
    case OpCode::LdC_i4_3:
    case OpCode::LdC_i4_4:
    case OpCode::LdC_i4_5:
    case OpCode::LdC_i4_6:
    case OpCode::LdC_i4_7:
    case OpCode::LdC_i4_8:
    case OpCode::Dup:
    case OpCode::Pop:
    case OpCode::Ret:
    case OpCode::Add_i4:
    case OpCode::Sub_i4:
    case OpCode::Mul_i4:
    case OpCode::Mul_u4:
    case OpCode::Div_i4:
    case OpCode::Div_u4:
    case OpCode::Rem_i4:
    case OpCode::Rem_u4:
    case OpCode::Add_i8:
    case OpCode::Sub_i8:
    case OpCode::Mul_i8:
    case OpCode::Mul_u8:
    case OpCode::Div_i8:
    case OpCode::Div_u8:
    case OpCode::Rem_i8:
    case OpCode::Rem_u8:
    case OpCode::Add_r4:
    case OpCode::Sub_r4:
    case OpCode::Mul_r4:
    case OpCode::Div_r4:
    case OpCode::Add_r8:
    case OpCode::Sub_r8:
    case OpCode::Mul_r8:
    case OpCode::Div_r8:
    case OpCode::And:
    case OpCode::Or:
    case OpCode::Xor:
    case OpCode::Neg:
    case OpCode::Not:
    case OpCode::Conv_i1:
    case OpCode::Conv_u1:
    case OpCode::Conv_i2:
    case OpCode::Conv_u2:
    case OpCode::Conv_i4:
    case OpCode::Conv_u4:
    case OpCode::Conv_i8:
    case OpCode::Conv_u8:
    case OpCode::Conv_r4:
    case OpCode::Conv_r8:
    case OpCode::Print:
        return print_simple_instruction(opcode, offset);
    case OpCode::Ld_Arg_S:
    case OpCode::Ld_Arg_i8_S:
    case OpCode::Ld_Loc_S:
    case OpCode::Ld_Loc_i8_S:
    case OpCode::St_Loc_S:
    case OpCode::St_Loc_i8_S:
        return print_arg_u8_instruction(opcode, offset);
    case OpCode::Ld_Loc:
    case OpCode::Ld_Loc_i8:
    case OpCode::St_Loc:
    case OpCode::St_Loc_i8:
        return print_arg_u16_instruction(opcode, offset);
    case OpCode::LdC_i4:
        return print_arg_u32_instruction(opcode, offset);
    case OpCode::LdC_i8:
        return print_arg_u64_instruction(opcode, offset);
    case OpCode::LdC_r4:
        return print_arg_f32_instruction(opcode, offset);
    case OpCode::LdC_r8:
        return print_arg_f64_instruction(opcode, offset);
    default:
        return print_simple_instruction(OpCode::Invalid, offset);
    }
}

u32 Chunk::print_simple_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    fmt::print("{}\n", g_opcode_str[(u32)opcode]);
    return offset + 1;
}

u32 Chunk::print_arg_u8_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u8 slot = m_bytecode[offset + 1];
    fmt::print("{:<16s} {:4d}\n", g_opcode_str[(u32)opcode], slot);
    return offset + 2;
}

u32 Chunk::print_arg_u16_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u16 value = (u16) (m_bytecode[offset + 1] << 8);
    value |= m_bytecode[offset + 2];
    fmt::print("{:<16s} {:4d}\n", g_opcode_str[(u32)opcode], value);
    return offset + 3;
}

u32 Chunk::print_arg_u32_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u32 value = get_u32_from_bytecode_offset(offset + 1);
    fmt::print("{:<16s} {:4d}\n", g_opcode_str[(u32)opcode], value);
    return offset + 5;
}

u32 Chunk::print_arg_u64_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u64 value = get_u64_from_bytecode_offset(offset + 1);
    fmt::print("{:<16s} {:4d}\n", g_opcode_str[(u32)opcode], value);
    return offset + 9;
}

u32 Chunk::print_arg_f32_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u32 value = get_u32_from_bytecode_offset(offset + 1);
    fmt::print("{:<16s} {:4d}\n", g_opcode_str[(u32)opcode], value);
    return offset + 5;
}

u32 Chunk::print_arg_f64_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u64 value = get_u64_from_bytecode_offset(offset + 1);
    f64 value_f;
    memcpy(&value_f, &value, sizeof(u64));
    fmt::print("{:<16s} {:8f}\n", g_opcode_str[(u32)opcode], value_f);
    return offset + 9;
}

u32 Chunk::print_string_instruction(OpCode opcode, u32 offset) {
    u32 string_loc = get_u32_from_bytecode_offset(offset + 1);

    fmt::print("{:<16s} {:4d} '", g_opcode_str[(u32)opcode], string_loc);
    auto str = m_constant_table.get_string(string_loc);
    fputs(str.data(), stdout);
    fputs("'\n", stdout);
    return offset + 5;
}

}

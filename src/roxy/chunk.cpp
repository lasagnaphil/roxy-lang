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
    case OpCode::nop:
    case OpCode::brk:
    case OpCode::ildarg_0:
    case OpCode::ildarg_1:
    case OpCode::ildarg_2:
    case OpCode::ildarg_3:
    case OpCode::ildloc_0:
    case OpCode::ildloc_1:
    case OpCode::ildloc_2:
    case OpCode::ildloc_3:
    case OpCode::istloc_0:
    case OpCode::istloc_1:
    case OpCode::istloc_2:
    case OpCode::istloc_3:
    case OpCode::lldarg_0:
    case OpCode::lldarg_1:
    case OpCode::lldarg_2:
    case OpCode::lldarg_3:
    case OpCode::lldloc_0:
    case OpCode::lldloc_1:
    case OpCode::lldloc_2:
    case OpCode::lldloc_3:
    case OpCode::lstloc_0:
    case OpCode::lstloc_1:
    case OpCode::lstloc_2:
    case OpCode::lstloc_3:
    case OpCode::ldcnil:
    case OpCode::ildc_m1:
    case OpCode::ildc_0:
    case OpCode::ildc_1:
    case OpCode::ildc_2:
    case OpCode::ildc_3:
    case OpCode::ildc_4:
    case OpCode::ildc_5:
    case OpCode::ildc_6:
    case OpCode::ildc_7:
    case OpCode::ildc_8:
    case OpCode::dup:
    case OpCode::pop:
    case OpCode::ret:
    case OpCode::iadd:
    case OpCode::isub:
    case OpCode::imul:
    case OpCode::uimul:
    case OpCode::idiv:
    case OpCode::uidiv:
    case OpCode::irem:
    case OpCode::uirem:
    case OpCode::ladd:
    case OpCode::lsub:
    case OpCode::lmul:
    case OpCode::ulmul:
    case OpCode::ldiv:
    case OpCode::uldiv:
    case OpCode::lrem:
    case OpCode::ulrem:
    case OpCode::fadd:
    case OpCode::fsub:
    case OpCode::fmul:
    case OpCode::fdiv:
    case OpCode::dadd:
    case OpCode::dsub:
    case OpCode::dmul:
    case OpCode::ddiv:
    case OpCode::band:
    case OpCode::bor:
    case OpCode::bxor:
    case OpCode::bneg:
    case OpCode::bnot:
    case OpCode::conv_i1:
    case OpCode::conv_u1:
    case OpCode::conv_i2:
    case OpCode::conv_u2:
    case OpCode::conv_i4:
    case OpCode::conv_u4:
    case OpCode::conv_i8:
    case OpCode::conv_u8:
    case OpCode::conv_r4:
    case OpCode::conv_r8:
    case OpCode::print:
        return print_simple_instruction(opcode, offset);
    case OpCode::ildarg_s:
    case OpCode::lldarg_s:
    case OpCode::ildloc_s:
    case OpCode::lldloc_s:
    case OpCode::istloc_s:
    case OpCode::lstloc_s:
        return print_arg_u8_instruction(opcode, offset);
    case OpCode::ildloc:
    case OpCode::lldloc:
    case OpCode::istloc:
    case OpCode::lstloc:
        return print_arg_u16_instruction(opcode, offset);
    case OpCode::ildc:
        return print_arg_u32_instruction(opcode, offset);
    case OpCode::lldc:
        return print_arg_u64_instruction(opcode, offset);
    case OpCode::fldc:
        return print_arg_f32_instruction(opcode, offset);
    case OpCode::dldc:
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

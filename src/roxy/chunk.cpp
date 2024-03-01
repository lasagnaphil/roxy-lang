#include "roxy/chunk.hpp"

#include <opcode.hpp>

#include "roxy/core/binary_search.hpp"
#include "roxy/fmt/core.h"
#include "roxy/module.hpp"

namespace rx {

Chunk::Chunk(std::string name, Module* outer_module) :
        m_name(std::move(name)), m_outer_module(outer_module),
        m_function_table(nullptr), m_native_function_table(nullptr) {}

void Chunk::write(u8 byte, u32 line) {
    m_lines.push_back(line);
    m_bytecode.push_back(byte);
}

void Chunk::find_ref_local_offsets() {
    for (auto& local : m_local_table) {
        if (local.type->kind == TypeKind::Primitive) {
            auto prim_kind = static_cast<PrimitiveTypeData*>(local.type.get())->prim_kind;
            if (prim_kind == PrimTypeKind::String) {
                m_ref_local_offsets.push_back(local.start);
            }
        }
    }
}

u32 Chunk::get_line(u32 bytecode_offset) {
    return m_lines[bytecode_offset];
}

void Chunk::print_disassembly() {
    fmt::print("== {} ==\n", m_name);
    for (i32 offset = 0; offset < m_bytecode.size();) {
        offset = disassemble_instruction(offset);
    }
}

u32 Chunk::disassemble_instruction(u32 offset) {
    static u32 prev_line = 0; // TODO: this is just a quick hack

    fmt::print("{:04d} ", offset);
    u32 cur_line = get_line(offset);
    if (offset > 0 && cur_line == prev_line) {
        fmt::print("   | ");
    }
    else {
        if (cur_line == 0xffffffff) {
            fmt::print("   . ", cur_line);
        }
        else {
            fmt::print("{:4d} ", cur_line);
        }
    }
    prev_line = cur_line;

    OpCode opcode = (OpCode)m_bytecode[offset];
    switch (opcode) {
    case OpCode::nop:
    case OpCode::brk:
    case OpCode::iload_0:
    case OpCode::iload_1:
    case OpCode::iload_2:
    case OpCode::iload_3:
    case OpCode::istore_0:
    case OpCode::istore_1:
    case OpCode::istore_2:
    case OpCode::istore_3:
    case OpCode::lload_0:
    case OpCode::lload_1:
    case OpCode::lload_2:
    case OpCode::lload_3:
    case OpCode::lstore_0:
    case OpCode::lstore_1:
    case OpCode::lstore_2:
    case OpCode::lstore_3:
    case OpCode::rload_0:
    case OpCode::rload_1:
    case OpCode::rload_2:
    case OpCode::rload_3:
    case OpCode::rstore_0:
    case OpCode::rstore_1:
    case OpCode::rstore_2:
    case OpCode::rstore_3:
    case OpCode::iconst_nil:
    case OpCode::iconst_m1:
    case OpCode::iconst_0:
    case OpCode::iconst_1:
    case OpCode::iconst_2:
    case OpCode::iconst_3:
    case OpCode::iconst_4:
    case OpCode::iconst_5:
    case OpCode::iconst_6:
    case OpCode::iconst_7:
    case OpCode::iconst_8:
    case OpCode::dup:
    case OpCode::pop:
    case OpCode::ret:
    case OpCode::iret:
    case OpCode::lret:
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
    case OpCode::lcmp:
    case OpCode::fcmp:
    case OpCode::dcmp:
    case OpCode::band:
    case OpCode::bor:
    case OpCode::bxor:
    case OpCode::bneg:
    case OpCode::bnot:
        return print_simple_instruction(opcode, offset);
    case OpCode::iload_s:
    case OpCode::lload_s:
    case OpCode::rload_s:
    case OpCode::istore_s:
    case OpCode::lstore_s:
    case OpCode::rstore_s:
    case OpCode::iconst_s:
        return print_arg_u8_instruction(opcode, offset);
    case OpCode::iload:
    case OpCode::lload:
    case OpCode::rload:
    case OpCode::istore:
    case OpCode::lstore:
    case OpCode::rstore:
    case OpCode::call:
    case OpCode::callnative:
        return print_arg_u16_instruction(opcode, offset);
    case OpCode::iconst:
        return print_arg_u32_instruction(opcode, offset);
    case OpCode::lconst:
        return print_arg_u64_instruction(opcode, offset);
    case OpCode::fconst:
        return print_arg_f32_instruction(opcode, offset);
    case OpCode::dconst:
        return print_arg_f64_instruction(opcode, offset);
    case OpCode::jmp_s:
    case OpCode::br_false_s:
    case OpCode::br_true_s:
    case OpCode::br_icmpeq_s:
    case OpCode::br_icmpne_s:
    case OpCode::br_icmpge_s:
    case OpCode::br_icmpgt_s:
    case OpCode::br_icmple_s:
    case OpCode::br_icmplt_s:
    case OpCode::br_eq_s:
    case OpCode::br_ne_s:
    case OpCode::br_ge_s:
    case OpCode::br_gt_s:
    case OpCode::br_le_s:
    case OpCode::br_lt_s:
        return print_branch_shortened_instruction(opcode, 1, offset);
    case OpCode::loop_s:
        return print_branch_shortened_instruction(opcode, -1, offset);
    case OpCode::jmp:
    case OpCode::br_false:
    case OpCode::br_true:
    case OpCode::br_icmpeq:
    case OpCode::br_icmpne:
    case OpCode::br_icmpge:
    case OpCode::br_icmpgt:
    case OpCode::br_icmple:
    case OpCode::br_icmplt:
    case OpCode::br_eq:
    case OpCode::br_ne:
    case OpCode::br_ge:
    case OpCode::br_gt:
    case OpCode::br_le:
    case OpCode::br_lt:
        return print_branch_instruction(opcode, 1, offset);
    case OpCode::loop:
        return print_branch_instruction(opcode, -1, offset);
    case OpCode::ldstr:
        return print_string_instruction(opcode, offset);
    default:
        return print_simple_instruction(OpCode::invalid, offset);
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

    u16 value = get_u16_from_bytecode_offset(offset + 1);
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

    u32 value_u32 = get_u32_from_bytecode_offset(offset + 1);
    f32 value;
    memcpy(&value, &value_u32, sizeof(f32));
    fmt::print("{:<16s} {:4.4f}\n", g_opcode_str[(u32)opcode], value);
    return offset + 5;
}

u32 Chunk::print_arg_f64_instruction(OpCode opcode, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);

    u64 value_u64 = get_u64_from_bytecode_offset(offset + 1);
    f64 value;
    memcpy(&value, &value_u64, sizeof(u64));
    fmt::print("{:<16s} {:4.4f}\n", g_opcode_str[(u32)opcode], value);
    return offset + 9;
}

u32 Chunk::print_branch_instruction(OpCode opcode, i32 sign, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);
    u32 jump = get_u32_from_bytecode_offset(offset + 1);
    fmt::print("{:<16s} {:4d} -> {:d}\n", g_opcode_str[(u32)opcode], offset, offset + 5 + sign * jump);
    return offset + 5;
}

u32 Chunk::print_branch_shortened_instruction(OpCode opcode, i32 sign, u32 offset) {
    assert((u32)opcode < (u32)OpCode::_count);
    u32 jump = (u32)m_bytecode[offset + 1];
    fmt::print("{:<16s} {:4d} -> {:d}\n", g_opcode_str[(u32)opcode], offset, offset + 2 + sign * jump);
    return offset + 2;
}

u32 Chunk::print_string_instruction(OpCode opcode, u32 offset) {
    u32 string_loc = get_u32_from_bytecode_offset(offset + 1);

    fmt::print("{:<16s} {:4d} '", g_opcode_str[(u32)opcode], string_loc);
    auto str = m_outer_module->string_table().get_string(string_loc);
    fputs(str.data(), stdout);
    fputs("'\n", stdout);
    return offset + 5;
}

}

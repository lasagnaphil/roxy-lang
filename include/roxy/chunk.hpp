#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/opcode.hpp"
#include "roxy/type.hpp"

#include <string_view>

namespace rx {
class Obj;

struct UntypedValue {
    union {
        bool value_bool;
        u8 value_u8;
        u16 value_u16;
        u32 value_u32;
        u64 value_u64;
        i8 value_i8;
        i16 value_i16;
        i32 value_i32;
        i64 value_i64;
        f32 value_f32;
        f64 value_f64;
        Obj* obj;
        u8 bytes[8];
    };
};

class ConstantTable {
public:
    u32 add_string(std::string_view str) {
        u32 offset = (u32)m_string_buf.size();
        m_string_buf += str;
        return offset;
    }

    u32 add_value(UntypedValue value) {
        u32 offset = m_values.size();
        m_values.push_back(value);
        return offset;
    }

    std::string_view get_string(u32 offset) {
        i32 term_loc = m_string_buf.find('\0', offset);
        return {m_string_buf.data() + offset, term_loc - offset};
    }

private:
    std::string m_string_buf;
    Vector<UntypedValue> m_values;
};

struct LocalTableEntry {
    u16 start;
    u16 size;
    TypeKind type_kind;
    PrimTypeKind prim_type_kind;
    std::string name;
};

struct Chunk {
    std::string m_name;
    Vector<u8> m_bytecode;
    ConstantTable m_constant_table;
    Vector<LocalTableEntry> m_local_table;
    Vector<Chunk> m_function_table;

    // Line debug information
    Vector<u32> m_lines;

    Chunk() = default;
    Chunk(std::string name) : m_name(std::move(name)) {}

    void write(u8 byte, u32 line);

    u32 add_string(std::string_view str);
    u32 add_constant(UntypedValue value);

    u32 get_locals_slot_size() {
        if (m_local_table.empty()) return 0;
        auto& last_local = m_local_table[m_local_table.size() - 1];
        return last_local.start + last_local.size;
    }

    u32 get_line(u32 bytecode_offset);

    void print_disassembly();
    u32 disassemble_instruction(u32 offset);

private:
    u32 print_simple_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u8_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u16_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u32_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u64_instruction(OpCode opcode, u32 offset);
    u32 print_arg_f32_instruction(OpCode opcode, u32 offset);
    u32 print_arg_f64_instruction(OpCode opcode, u32 offset);
    u32 print_branch_instruction(OpCode opcode, i32 sign, u32 offset);
    u32 print_branch_shortened_instruction(OpCode opcode, i32 sign, u32 offset);
    u32 print_string_instruction(OpCode opcode, u32 offset);

    u16 get_u16_from_bytecode_offset(u32 offset) {
        u16 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u16));
        return value;
    }

    u32 get_u32_from_bytecode_offset(u32 offset) {
        u32 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u32));
        return value;
    }

    u64 get_u64_from_bytecode_offset(u32 offset) {
        u64 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u64));
        return value;
    }
};
}

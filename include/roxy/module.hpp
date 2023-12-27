#pragma once

#include "roxy/chunk.hpp"

namespace rx {

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

struct FunctionTableEntry {
    std::string name;
    // Vector<std::string> param_names;
    FunctionTypeData type;
    UniquePtr<Chunk> chunk;
};

class Module {
    friend class Compiler;

public:
    Module(std::string name) : m_name(std::move(name)), m_chunk(m_name, this) {}

    Chunk& chunk() { return m_chunk; }
    ConstantTable& constant_table() { return m_constant_table; }

    u32 add_string(std::string_view str);
    u32 add_constant(UntypedValue value);

    void print_disassembly();

private:

    void build_for_runtime();

    std::string m_name;
    Chunk m_chunk;
    ConstantTable m_constant_table;
    Vector<FunctionTableEntry> m_function_table;
    Vector<StructTypeData> m_struct_table;

    Vector<Chunk*> m_runtime_function_table;
};

}
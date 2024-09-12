#pragma once

#include "roxy/core/array.hpp"
#include "roxy/chunk.hpp"

namespace rx {

class StringTable {
public:
    u32 add_string(std::string_view str) {
        u32 offset = (u32)m_string_buf.size();
        m_string_buf += str;
        m_string_buf += '\0';
        return offset;
    }

    std::string_view get_string(u32 offset) {
        i32 term_loc = m_string_buf.find('\0', offset);
        return {m_string_buf.data() + offset, term_loc - offset};
    }

private:
    std::string m_string_buf;
};

struct FunctionTableEntry {
    std::string name;
    std::string module;
    FunctionTypeData type;
    UniquePtr<Chunk> chunk;
};

class ArgStack {
public:
    ArgStack(u32* stack_top) {
        m_stack_top = stack_top;
    }

    inline void push_u32(u32 value) {
        *m_stack_top = value;
        m_stack_top++;
    }

    inline void push_u64(u64 value) {
        m_stack_top[0] = value;
        m_stack_top[1] = value >> 32;
        m_stack_top += 2;
    }

    inline void push_f32(f32 value) {
        u32 value_u32;
        memcpy(&value_u32, &value, sizeof(u32));
        push_u32(value_u32);
    }

    inline void push_f64(f64 value) {
        u64 value_u64;
        memcpy(&value_u64, &value, sizeof(u64));
        push_u64(value_u64);
    }

    inline void push_ref(Obj* ref) {
        push_u64(reinterpret_cast<intptr_t>(ref));
    }

    inline u32* top() { return m_stack_top; }

    inline i32 pop_i32() { return (i32)pop_u32(); }
    inline i64 pop_i64() { return (i64)pop_u64(); }

    inline u32 pop_u32() {
        m_stack_top--;
        return *m_stack_top;
    }

    inline u64 pop_u64() {
        m_stack_top -= 2;
        u64 value = (u64)m_stack_top[0];
        value |= (u64)m_stack_top[1] << 32;
        return value;
    }

    inline f32 pop_f32() {
        u32 value = pop_u32();
        f32 value_f32;
        memcpy(&value_f32, &value, sizeof(f32));
        return value_f32;
    }

    inline f64 pop_f64() {
        u64 value = pop_u64();
        f64 value_f64;
        memcpy(&value_f64, &value, sizeof(f64));
        return value_f64;
    }

    inline Obj* pop_ref() {
        u64 value = pop_u64();
        return reinterpret_cast<Obj*>(static_cast<intptr_t>(value));
    }

private:
    u32* m_stack_top;
};

struct NativeFunctionTableEntry {
    std::string name;
    std::string module;
    FunctionTypeData type;
    NativeFunctionRef fun;
};

class Module {
    friend class Compiler;
    friend class ILCompiler;
    friend class Library;

public:
    Module(std::string name, const u8* source) :
        m_name(std::move(name)), m_chunk(m_name, this), m_source(source) {}

    std::string_view name() const { return m_name; }
    const u8* source() const { return m_source; }
    Chunk& chunk() { return m_chunk; }
    StringTable& string_table() { return m_string_table; }

    bool add_native_function(std::string_view name, NativeFunctionRef fun);

    u16 find_native_function_index(std::string_view name);

    void print_disassembly();

private:

    std::string m_name;
    const u8* m_source;
    Chunk m_chunk;
    StringTable m_string_table;
    Vector<FunctionTableEntry> m_function_table;
    Vector<NativeFunctionTableEntry> m_native_function_table;
    Vector<StructTypeData> m_struct_table;

    Vector<Chunk*> m_runtime_function_table;
    Vector<NativeFunctionRef> m_runtime_native_fun_table;

    static UniquePtr<Module> s_builtin_module;
};

}
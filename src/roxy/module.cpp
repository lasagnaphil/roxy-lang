#include "roxy/module.hpp"
#include "roxy/fmt/core.h"

namespace rx {

u32 Module::add_string(std::string_view str) {
    return m_constant_table.add_string(str);
}

u32 Module::add_constant(UntypedValue value) {
    return m_constant_table.add_value(value);
}

void Module::print_disassembly() {
    m_chunk.print_disassembly();
    fmt::print("\n");
    for (auto& fn_entry : m_function_table) {
        fmt::print("Functions: \n", fn_entry.name);
        fn_entry.chunk->print_disassembly();
    }
}

void Module::build_for_runtime() {
    m_runtime_function_table.reserve(m_function_table.size());

    for (auto& fn_entry : m_function_table) {
        m_runtime_function_table.push_back(fn_entry.chunk.get());
    }

    m_chunk.m_function_table = m_runtime_function_table.data();
    for (auto& fun_entry: m_function_table) {
        fun_entry.chunk->m_function_table = m_runtime_function_table.data();
    }
}

}
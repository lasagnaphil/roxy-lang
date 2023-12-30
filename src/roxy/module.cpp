#include "roxy/module.hpp"
#include "roxy/fmt/core.h"
#include "roxy/object.hpp"
#include "roxy/string.hpp"

namespace rx {

// TODO: make this O(1) instead of O(N)
bool Module::add_native_function(std::string_view name, NativeFunctionRef fun) {
    auto found_entry = m_native_function_table.find_if([=](auto& entry) { return entry.name == name; });
    if (found_entry) {
        found_entry->fun = fun;
        return true;
    }
    else {
        return false;
    }
}

// TODO: Make this O(1) instead of O(N)
u16 Module::find_native_function_index(std::string_view name) {
    auto found_entry = m_native_function_table.find_if([=](auto& entry) { return entry.name == name; });
    if (found_entry) {
        return found_entry - m_native_function_table.data();
    }
    else {
        return UINT16_MAX;
    }
}

void Module::print_disassembly() {
    m_chunk.print_disassembly();
    fmt::print("\nFunctions: \n");
    for (auto& fn_entry : m_function_table) {
        if (fn_entry.chunk.get()) {
            fn_entry.chunk->print_disassembly();
        }
    }
}

}
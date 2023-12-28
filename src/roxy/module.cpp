#include "roxy/module.hpp"
#include "roxy/fmt/core.h"

namespace rx {

static const char* builtin_module_src = R"(
native fun print_i32(value: i32);
native fun print_i64(value: i64);
native fun print_u32(value: u32);
native fun print_u64(value: u64);
native fun print_float(value: float);
native fun print_double(value: double);
)";

UniquePtr<Module> Module::s_builtin_module = nullptr;

bool Module::add_native_function(std::string_view name, NativeFunctionRef fun) {
    // TODO: make this O(1) instead of O(N)
    auto found_entry = m_native_function_table.find_if([=](auto& entry) { return entry.name == name; });
    if (found_entry) {
        found_entry->fun = fun;
        return true;
    }
    else {
        return false;
    }
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
    if (s_builtin_module.get() == nullptr) {
        s_builtin_module = UniquePtr<Module>(new Module("builtin", reinterpret_cast<const u8*>(builtin_module_src)));
        s_builtin_module->load_basic_module();
    }

    // just a placeholder test...
    load_basic_module();

    m_runtime_function_table.reserve(m_function_table.size());
    for (auto& fn_entry : m_function_table) {
        m_runtime_function_table.push_back(fn_entry.chunk.get());
    }

    m_runtime_native_fun_table.reserve(m_native_function_table.size());
    for (auto& fn_entry : m_native_function_table) {
        m_runtime_native_fun_table.push_back(fn_entry.fun);
    }

    m_chunk.m_function_table = m_runtime_function_table.data();
    m_chunk.m_native_function_table = m_runtime_native_fun_table.data();
    for (auto& fun_entry : m_function_table) {
        fun_entry.chunk->m_function_table = m_runtime_function_table.data();
        fun_entry.chunk->m_native_function_table = m_runtime_native_fun_table.data();
    }
}

#define ADD_NATIVE_PRINT_FUN(Type, FormatStr) \
add_native_function("print_" #Type, [](ArgStack* args) { \
    Type value = args->pop_##Type(); \
    printf(FormatStr "\n", value); \
});

void Module::load_basic_module() {
    ADD_NATIVE_PRINT_FUN(i32, "%d")
    ADD_NATIVE_PRINT_FUN(i64, "%lld")
    ADD_NATIVE_PRINT_FUN(u32, "%u")
    ADD_NATIVE_PRINT_FUN(u64, "%llu")
    ADD_NATIVE_PRINT_FUN(f32, "%f")
    ADD_NATIVE_PRINT_FUN(f64, "%f")
}

#undef ADD_NATIVE_PRINT_FUN

}
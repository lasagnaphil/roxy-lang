#include "roxy/module.hpp"
#include "roxy/fmt/core.h"
#include "roxy/object.hpp"
#include "roxy/string.hpp"

namespace rx {

static const char* builtin_module_src = R"(
native fun print_i32(value: i32);
native fun print_i64(value: i64);
native fun print_u32(value: u32);
native fun print_u64(value: u64);
native fun print_float(value: float);
native fun print_double(value: double);
native fun print(value: string);
native fun concat(a: string, b: string);

native fun clock(): double;
)";

UniquePtr<Module> Module::s_builtin_module = nullptr;

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

    add_native_function("print", [](ArgStack* args) {
        ObjString* obj = reinterpret_cast<ObjString*>(args->pop_ref());
        puts(obj->chars());
    });

    add_native_function("concat", [](ArgStack* args) {
        ObjString* b = reinterpret_cast<ObjString*>(args->pop_ref());
        ObjString* a = reinterpret_cast<ObjString*>(args->pop_ref());
        ObjString* res = ObjString::concat(a, b);
        args->push_ref(reinterpret_cast<Obj*>(res));
    });

    add_native_function("clock", [](ArgStack* args) {
        args->push_f64((f64)clock() / CLOCKS_PER_SEC);
    });
}

#undef ADD_NATIVE_PRINT_FUN

}
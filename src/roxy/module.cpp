#include "roxy/module.hpp"
#include "roxy/fmt/core.h"

static const char* builtin_module_src = R"(
native fun print_int(value: int);
)";

namespace rx {

u32 Module::add_native_function(NativeFunctionTableEntry entry) {
    u32 index = m_native_function_table.size();
    m_native_function_table.push_back(std::move(entry));
    return index;
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

void Module::load_basic_module() {
    FunctionTypeData type_print_int;
    type_print_int.params.push_back(UniquePtr<TypeData>(new PrimitiveTypeData(PrimTypeKind::U32)));
    type_print_int.ret = UniquePtr<TypeData>(new PrimitiveTypeData(PrimTypeKind::Void));

    NativeFunctionTableEntry print_int = {
            .name = "print_int",
            .type = std::move(type_print_int),
            .fun = [](ArgStack& args) {
                u32 value = args.pop_u32();
                printf("%d\n", value);
            }
    };
    add_native_function(std::move(print_int));
}

}
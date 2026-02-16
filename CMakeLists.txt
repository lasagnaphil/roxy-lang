cmake_minimum_required(VERSION 3.16)
project(roxy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AddressSanitizer option
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address -fno-omit-frame-pointer)
    if(WIN32)
        # Link ASAN runtime for clang-cl on Windows
        link_libraries(clang_rt.asan_dynamic-x86_64 clang_rt.asan_dynamic_runtime_thunk-x86_64)
        add_link_options(/wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
        # ASAN on Windows requires release CRT (not debug)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreadedDLL")
        # Copy ASAN DLL to build directory
        file(GLOB ASAN_DLL "C:/Program Files/LLVM/lib/clang/*/lib/windows/clang_rt.asan_dynamic-x86_64.dll")
        if(ASAN_DLL)
            file(COPY "${ASAN_DLL}" DESTINATION "${CMAKE_BINARY_DIR}")
            message(STATUS "Copied ASAN DLL to build directory: ${ASAN_DLL}")
        else()
            message(WARNING "ASAN DLL not found, you may need to copy it manually")
        endif()
    else()
        # macOS/Linux: just add linker flag
        add_link_options(-fsanitize=address)
    endif()
endif()

# Include directories
include_directories(include)

# Core library
add_library(roxy_core
    src/roxy/core/file.cpp
    src/roxy/core/string.cpp
)

# Shared library (lexer, tokens)
add_library(roxy_shared
    src/roxy/shared/token_kinds.cpp
    src/roxy/shared/lexer.cpp
)
target_link_libraries(roxy_shared roxy_core)

# Compiler library (parser, AST, semantic analysis, SSA IR)
add_library(roxy_compiler
    src/roxy/compiler/parser.cpp
    src/roxy/compiler/types.cpp
    src/roxy/compiler/symbol_table.cpp
    src/roxy/compiler/semantic.cpp
    src/roxy/compiler/ssa_ir.cpp
    src/roxy/compiler/ir_builder.cpp
    src/roxy/compiler/generics.cpp
    src/roxy/compiler/module_registry.cpp
    src/roxy/compiler/compiler.cpp
)
target_link_libraries(roxy_compiler roxy_shared)

# VM library (bytecode, value, object, vm, interpreter, lowering, list, string, natives)
add_library(roxy_vm
    src/roxy/vm/bytecode.cpp
    src/roxy/vm/value.cpp
    src/roxy/vm/object.cpp
    src/roxy/vm/vm.cpp
    src/roxy/vm/interpreter.cpp
    src/roxy/vm/list.cpp
    src/roxy/vm/string.cpp
    src/roxy/vm/natives.cpp
    src/roxy/vm/slab_allocator.cpp
    src/roxy/compiler/lowering.cpp
)

# Platform-specific virtual memory implementations
if(WIN32)
    target_sources(roxy_vm PRIVATE src/roxy/vm/vmem_win32.cpp)
else()
    target_sources(roxy_vm PRIVATE src/roxy/vm/vmem_unix.cpp)
endif()

target_link_libraries(roxy_vm roxy_compiler)

# Single unified test executable
add_executable(roxy_tests
    # Main entry point
    tests/test_main.cpp

    # Unit tests
    tests/unit/lexer_test.cpp
    tests/unit/parser_test.cpp
    tests/unit/semantic_test.cpp
    tests/unit/ssa_ir_test.cpp
    tests/unit/bytecode_test.cpp
    tests/unit/vm_test.cpp
    tests/unit/lowering_test.cpp
    tests/unit/slab_allocator_test.cpp
    tests/unit/format_test.cpp
    tests/unit/string_test.cpp

    # E2E tests
    tests/e2e/test_helpers.cpp
    tests/e2e/basics_test.cpp
    tests/e2e/recursion_test.cpp
    tests/e2e/algorithms_test.cpp
    tests/e2e/lists_test.cpp
    tests/e2e/structs_test.cpp
    tests/e2e/params_test.cpp
    tests/e2e/interop_test.cpp
    tests/e2e/strings_test.cpp
    tests/e2e/heap_test.cpp
    tests/e2e/modules_test.cpp
    tests/e2e/constructors_test.cpp
    tests/e2e/methods_test.cpp
    tests/e2e/inheritance_test.cpp
    tests/e2e/type_inference_test.cpp
    tests/e2e/casts_test.cpp
    tests/e2e/enums_test.cpp
    tests/e2e/when_test.cpp
    tests/e2e/tagged_unions_test.cpp
    tests/e2e/traits_test.cpp
    tests/e2e/generics_test.cpp
)
target_link_libraries(roxy_tests roxy_vm)

# Standalone interpreter executable
add_executable(roxy src/roxy.cpp)
target_link_libraries(roxy roxy_vm)

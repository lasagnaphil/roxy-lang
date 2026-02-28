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
    src/roxy/core/json.cpp
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
    src/roxy/compiler/ir_validator.cpp
    src/roxy/compiler/generics.cpp
    src/roxy/compiler/module_registry.cpp
    src/roxy/compiler/compiler.cpp
    src/roxy/compiler/type_env.cpp
    src/roxy/compiler/coroutine_lowering.cpp
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
    src/roxy/vm/map.cpp
    src/roxy/vm/string.cpp
    src/roxy/vm/natives.cpp
    src/roxy/vm/slab_allocator.cpp
    src/roxy/vm/binding/registry.cpp
    src/roxy/compiler/lowering.cpp
)

# Platform-specific virtual memory implementations
if(WIN32)
    target_sources(roxy_vm PRIVATE src/roxy/vm/vmem_win32.cpp)
else()
    target_sources(roxy_vm PRIVATE src/roxy/vm/vmem_unix.cpp)
endif()

target_link_libraries(roxy_vm roxy_compiler)

# LSP library
add_library(roxy_lsp
    src/roxy/lsp/lsp_parser.cpp
    src/roxy/lsp/indexer.cpp
    src/roxy/lsp/global_index.cpp
    src/roxy/lsp/cst_lowering.cpp
    src/roxy/lsp/lsp_type_resolver.cpp
    src/roxy/lsp/transport.cpp
    src/roxy/lsp/server.cpp
)
target_link_libraries(roxy_lsp roxy_shared)

# LSP server executable
add_executable(roxy_lsp_server src/roxy_lsp_server.cpp)
target_link_libraries(roxy_lsp_server roxy_lsp)

# Single unified test executable
add_executable(roxy_tests
    # Main entry point
    tests/test_main.cpp

    # Unit tests
    tests/unit/test_lexer.cpp
    tests/unit/test_parser.cpp
    tests/unit/test_semantic.cpp
    tests/unit/test_ssa_ir.cpp
    tests/unit/test_bytecode.cpp
    tests/unit/test_vm.cpp
    tests/unit/test_lowering.cpp
    tests/unit/test_ir_validator.cpp
    tests/unit/test_slab_allocator.cpp
    tests/unit/test_format.cpp
    tests/unit/test_string.cpp
    tests/unit/test_static_string.cpp
    tests/unit/test_json.cpp
    tests/unit/test_lsp_parser.cpp
    tests/unit/test_indexer.cpp
    tests/unit/test_global_index.cpp
    tests/unit/test_cst_lowering.cpp
    tests/unit/test_lsp_type_resolver.cpp

    # E2E tests
    tests/e2e/test_helpers.cpp
    tests/e2e/test_basics.cpp
    tests/e2e/test_recursion.cpp
    tests/e2e/test_algorithms.cpp
    tests/e2e/test_lists.cpp
    tests/e2e/test_maps.cpp
    tests/e2e/test_structs.cpp
    tests/e2e/test_params.cpp
    tests/e2e/test_interop.cpp
    tests/e2e/test_strings.cpp
    tests/e2e/test_heap.cpp
    tests/e2e/test_raii.cpp
    tests/e2e/test_modules.cpp
    tests/e2e/test_constructors.cpp
    tests/e2e/test_methods.cpp
    tests/e2e/test_inheritance.cpp
    tests/e2e/test_type_inference.cpp
    tests/e2e/test_casts.cpp
    tests/e2e/test_enums.cpp
    tests/e2e/test_when.cpp
    tests/e2e/test_tagged_unions.cpp
    tests/e2e/test_traits.cpp
    tests/e2e/test_generics.cpp
    tests/e2e/test_int_literal.cpp
    tests/e2e/test_exceptions.cpp
    tests/e2e/test_coroutines.cpp
)
target_link_libraries(roxy_tests roxy_vm roxy_lsp)

# Standalone interpreter executable
add_executable(roxy src/roxy.cpp)
target_link_libraries(roxy roxy_vm)

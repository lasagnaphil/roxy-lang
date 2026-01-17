cmake_minimum_required(VERSION 3.16)
project(roxy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# AddressSanitizer option
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    add_compile_options(-fsanitize=address)
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
endif()

# Include directories
include_directories(include)

# Core library
add_library(roxy_core
    src/roxy/core/file.cpp
    src/roxy/core/fmt/format.cc
    src/roxy/core/fmt/os.cc
)

# Disable ASAN for fmt library files (known compatibility issue with clang-cl ASAN)
if(ENABLE_ASAN)
    set_source_files_properties(
        src/roxy/core/fmt/format.cc
        src/roxy/core/fmt/os.cc
        PROPERTIES COMPILE_FLAGS "-fno-sanitize=address"
    )
endif()

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
)
target_link_libraries(roxy_compiler roxy_shared)

# Lexer test executable
add_executable(lexer_test tests/lexer_test.cpp)
target_link_libraries(lexer_test roxy_shared)

# Parser test executable
add_executable(parser_test tests/parser_test.cpp)
target_link_libraries(parser_test roxy_compiler)

# Semantic test executable
add_executable(semantic_test tests/semantic_test.cpp)
target_link_libraries(semantic_test roxy_compiler)

# SSA IR test executable
add_executable(ssa_ir_test tests/ssa_ir_test.cpp)
target_link_libraries(ssa_ir_test roxy_compiler)

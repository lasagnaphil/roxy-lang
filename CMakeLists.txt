cmake_minimum_required(VERSION 3.16)
project(roxy)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Include directories
include_directories(include)

# Core library (header-only for now, but file.cpp exists)
add_library(roxy_core
    src/roxy/core/file.cpp
)

# Shared library (lexer, tokens)
add_library(roxy_shared
    src/roxy/shared/token_kinds.cpp
    src/roxy/shared/lexer.cpp
)
target_link_libraries(roxy_shared roxy_core)

# Compiler library (parser, AST)
add_library(roxy_compiler
    src/roxy/compiler/parser.cpp
)
target_link_libraries(roxy_compiler roxy_shared)

# Lexer test executable
add_executable(lexer_test tests/lexer_test.cpp)
target_link_libraries(lexer_test roxy_shared)

# Parser test executable
add_executable(parser_test tests/parser_test.cpp)
target_link_libraries(parser_test roxy_compiler)

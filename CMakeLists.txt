cmake_minimum_required(VERSION 3.26)
project(roxy_lang)

set(CMAKE_CXX_STANDARD 20)

add_library(roxy STATIC
        src/roxy/chunk.cpp
        src/roxy/opcode.cpp
        src/roxy/scanner.cpp
        src/roxy/parser.cpp
        src/roxy/type.cpp
        src/roxy/value.cpp
        src/roxy/object.cpp
        src/roxy/string.cpp
        src/roxy/string_interner.cpp
        src/roxy/sema.cpp
        src/roxy/vm.cpp
        src/roxy/core/file.cpp
        src/roxy/fmt/format.cc
)

target_include_directories(roxy PUBLIC include)
target_include_directories(roxy PRIVATE include/roxy)
target_compile_definitions(roxy PRIVATE FMT_EXCEPTIONS=0 XXH_INLINE_ALL)
target_compile_options(roxy PRIVATE -Wreturn-type -Wno-c99-designator)

add_executable(roxy_bin src/roxy-bin/main.cpp)
target_link_libraries(roxy_bin PRIVATE roxy)
target_compile_definitions(roxy_bin PRIVATE FMT_EXCEPTIONS=0)

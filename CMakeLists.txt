cmake_minimum_required(VERSION 3.26)
project(roxy_lang)

set(CMAKE_CXX_STANDARD 20)

add_library(roxy STATIC
        src/roxy/chunk.cpp
        src/roxy/module.cpp
        src/roxy/library.cpp
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

option (USE_ASAN_WINDOWS OFF)

# Windows-specific code for enabling ASAN.
if (USE_ASAN_WINDOWS)
    set(LLVM_DYNLIB_PATH "$ENV{ProgramFiles}/LLVM/lib/clang/16/lib/windows")
    target_compile_options(roxy_bin PRIVATE -fsanitize=address)
    target_link_directories(roxy_bin PRIVATE ${LLVM_DYNLIB_PATH})
    target_link_libraries(roxy_bin PRIVATE clang_rt.asan_dynamic-x86_64 clang_rt.asan_dynamic_runtime_thunk-x86_64)
    target_link_options(roxy_bin PRIVATE /wholearchive:clang_rt.asan_dynamic_runtime_thunk-x86_64.lib)
    target_compile_definitions(roxy_bin PRIVATE _DISABLE_VECTOR_ANNOTATION _DISABLE_STRING_ANNOTATION)
    add_custom_command(
            TARGET roxy_bin POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy
            ${LLVM_DYNLIB_PATH}/clang_rt.asan_dynamic-x86_64.dll
            ${CMAKE_CURRENT_BINARY_DIR}/clang_rt.asan_dynamic-x86_64.dll)
endif()
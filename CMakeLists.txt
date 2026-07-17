cmake_minimum_required(VERSION 3.16)
project(roxy)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Bytecode profiler option — adds per-opcode count + TSC cycle accounting in
# the interpreter dispatch loop. Off by default (zero cost). When ON, the VM
# prints a sorted opcode summary on destroy. Build under build-profile/ and
# run any .roxy program; useful for finding hot opcodes when optimizing.
option(ENABLE_BC_PROFILE "Enable bytecode profiler (per-opcode count + cycles)" OFF)
if(ENABLE_BC_PROFILE)
    message(STATUS "Bytecode profiler enabled")
    add_compile_definitions(ROXY_PROFILE_BYTECODE=1)
endif()

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

# Fuzzing option — builds the libFuzzer targets in tests/fuzz/ (lexer, parser,
# LSP parser). Requires a Clang toolchain that ships the libFuzzer runtime
# (Homebrew LLVM, upstream LLVM, or Windows LLVM — NOT Apple clang, which omits
# libclang_rt.fuzzer). Configure a dedicated build dir with that compiler:
#
#   cmake -B build-fuzz -G Ninja -DENABLE_FUZZERS=ON \
#     -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
#     -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
#   ninja -C build-fuzz fuzz_lexer fuzz_parser fuzz_lsp_parser
#
# All TUs are coverage-instrumented (fuzzer-no-link) so the fuzzer sees into the
# lexer/parser/LSP libraries; each fuzz executable additionally links the
# libFuzzer driver. UBSan is enabled (aborts on UB, so libFuzzer flags it as a
# crash). ASAN is intentionally left to the existing ENABLE_ASAN flag so the two
# compose (ASAN is currently broken on macOS Tahoe — see CLAUDE.md). Asserts
# stay on (no NDEBUG unless a Release build type is set). See tests/fuzz/README.md.
option(ENABLE_FUZZERS "Build libFuzzer fuzz targets (requires Clang with the libFuzzer runtime)" OFF)
if(ENABLE_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "ENABLE_FUZZERS requires a Clang toolchain (got '${CMAKE_CXX_COMPILER_ID}')")
    endif()
    message(STATUS "Fuzzers enabled (libFuzzer coverage + UBSan)")
    add_compile_options(-fsanitize=fuzzer-no-link,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer -g -O1)
    add_link_options(-fsanitize=undefined)
endif()

# Tracy profiler client — vendored as a submodule (third_party/tracy) so it builds
# cross-platform from one source (no system package needed). OFF by default and
# zero-overhead: the ROXY_ZONE/ROXY_FRAME_MARK macros in core/trace.hpp compile to
# nothing unless this is ON. When ON, capture traces headless with `tracy-capture`
# and export stats with `tracy-csvexport` (both built from the same submodule) —
# see docs/internals/profiling.md. Must precede the target definitions below so
# link_libraries() propagates Tracy::TracyClient to every target.
option(ENABLE_TRACY "Build with Tracy profiler client instrumentation" OFF)
if(ENABLE_TRACY)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy/CMakeLists.txt")
        message(FATAL_ERROR "ENABLE_TRACY needs the Tracy submodule: "
                            "git submodule update --init third_party/tracy")
    endif()
    message(STATUS "Tracy profiler client enabled (on-demand)")
    # On-demand: an instrumented binary runs normally until a profiler connects,
    # so ENABLE_TRACY builds stay usable for everyday runs and tests.
    set(TRACY_ON_DEMAND ON CACHE BOOL "Tracy on-demand profiling" FORCE)
    add_subdirectory(third_party/tracy)
    add_compile_definitions(ROXY_TRACY=1)
    link_libraries(Tracy::TracyClient)
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
    src/roxy/compiler/type_checker.cpp
    src/roxy/compiler/lifetime_checker.cpp
    src/roxy/compiler/trait_system.cpp
    src/roxy/compiler/generic_call_resolver.cpp
    src/roxy/compiler/ssa_ir.cpp
    src/roxy/compiler/ir_fold.cpp
    src/roxy/compiler/ownership_tracker.cpp
    src/roxy/compiler/ir_builder.cpp
    src/roxy/compiler/ir_builder_stmt.cpp
    src/roxy/compiler/ir_builder_expr.cpp
    src/roxy/compiler/ir_builder_lifetime.cpp
    src/roxy/compiler/ir_validator.cpp
    src/roxy/compiler/ir_optimize.cpp
    src/roxy/compiler/generics.cpp
    src/roxy/compiler/module_registry.cpp
    src/roxy/compiler/compiler.cpp
    src/roxy/compiler/type_env.cpp
    src/roxy/compiler/coroutine_lowering.cpp
    src/roxy/compiler/c_emitter.cpp
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
    src/roxy/vm/map_dispatch.cpp
    src/roxy/vm/string.cpp
    src/roxy/vm/natives.cpp
    src/roxy/vm/binding/registry.cpp
    src/roxy/compiler/lowering.cpp
)

target_link_libraries(roxy_vm roxy_compiler roxy_rt)

# Runtime library (used by both AOT-compiled C backend binaries and the VM —
# RoxyVM embeds `roxy_ctx` and the interpreter sets the thread-local context
# via `roxy::ScopedContext` on every public entry; the slab allocator lives
# here so AOT-compiled programs get the same generation-based weak-ref
# soundness as VM-mode programs).
add_library(roxy_rt
    src/roxy/rt/roxy_rt.cpp
    src/roxy/rt/slab_allocator.cpp
    src/roxy/rt/string_intern.cpp
)

# Platform-specific virtual memory implementations (used by the slab allocator)
if(WIN32)
    target_sources(roxy_rt PRIVATE src/roxy/rt/vmem_win32.cpp)
else()
    target_sources(roxy_rt PRIVATE src/roxy/rt/vmem_unix.cpp)
endif()

# LSP library
add_library(roxy_lsp
    src/roxy/lsp/lsp_parser.cpp
    src/roxy/lsp/indexer.cpp
    src/roxy/lsp/global_index.cpp
    src/roxy/lsp/cst_lowering.cpp
    src/roxy/lsp/lsp_analysis_context.cpp
    src/roxy/lsp/transport.cpp
    src/roxy/lsp/server.cpp
)
target_link_libraries(roxy_lsp roxy_compiler roxy_vm)

# GNU ld / LLD resolve each archive in a single pass, so the bi-directional
# dependency between roxy_compiler and roxy_vm (e.g. semantic.cpp references
# NativeRegistry; compiler.cpp references register_builtin_natives) needs
# --start-group/--end-group to be resolvable. Apple's ld64 doesn't need this.
if(UNIX AND NOT APPLE)
    set(ROXY_LINK_START "-Wl,--start-group")
    set(ROXY_LINK_END "-Wl,--end-group")
else()
    set(ROXY_LINK_START "")
    set(ROXY_LINK_END "")
endif()

# LSP server executable
add_executable(roxy_lsp_server src/roxy_lsp_server.cpp)
target_link_libraries(roxy_lsp_server ${ROXY_LINK_START} roxy_lsp roxy_vm roxy_compiler roxy_shared roxy_core ${ROXY_LINK_END})

# Single unified test executable
add_executable(roxy_tests
    # Main entry point
    tests/test_main.cpp

    # Unit tests
    tests/unit/test_lexer.cpp
    tests/unit/test_parser.cpp
    tests/unit/test_semantic.cpp
    tests/unit/test_lifecycle_predicates.cpp
    tests/unit/test_ssa_ir.cpp
    tests/unit/test_bytecode.cpp
    tests/unit/test_vm.cpp
    tests/unit/test_lowering.cpp
    tests/unit/test_ir_validator.cpp
    tests/unit/test_ir_optimize.cpp
    tests/unit/test_slab_allocator.cpp
    tests/unit/test_format.cpp
    tests/unit/test_string.cpp
    tests/unit/test_static_string.cpp
    tests/unit/test_json.cpp
    tests/unit/test_lsp_parser.cpp
    tests/unit/test_indexer.cpp
    tests/unit/test_global_index.cpp
    tests/unit/test_cst_lowering.cpp
    tests/unit/test_lsp_completion.cpp
    tests/unit/test_lsp_hover.cpp
    tests/unit/test_lsp_references.cpp
    tests/unit/test_lsp_analysis_context.cpp
    tests/unit/test_runtime_ctx.cpp
    tests/unit/test_container_pin.cpp

    # Fuzz-harness regression replay (seed corpus + examples + inline cases).
    # The harness bodies are shared with the tests/fuzz/ libFuzzer executables.
    tests/unit/test_fuzz_regression.cpp
    tests/fuzz/fuzz_one_lexer.cpp
    tests/fuzz/fuzz_one_parser.cpp
    tests/fuzz/fuzz_one_lsp_parser.cpp

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
    tests/e2e/test_lifetimes.cpp
    tests/e2e/test_lifetime_regressions.cpp
    tests/e2e/test_globals.cpp
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
    tests/e2e/test_narrow_arith.cpp
    tests/e2e/test_unsigned_arith.cpp
    tests/e2e/test_exceptions.cpp
    tests/e2e/test_coroutines.cpp
    tests/e2e/test_closures.cpp
    tests/e2e/test_c_backend.cpp
    tests/e2e/test_recursive_types.cpp
    tests/e2e/test_lox.cpp
    tests/e2e/test_cli.cpp
)
target_link_libraries(roxy_tests ${ROXY_LINK_START} roxy_vm roxy_lsp roxy_compiler roxy_shared roxy_core ${ROXY_LINK_END})
target_compile_definitions(roxy_tests PRIVATE "ROXY_PROJECT_ROOT=\"${CMAKE_SOURCE_DIR}\"")

# Standalone interpreter executable
add_executable(roxy src/roxy.cpp)
target_link_libraries(roxy ${ROXY_LINK_START} roxy_vm roxy_compiler roxy_shared roxy_core ${ROXY_LINK_END})

# The driver's own argv handling lives in src/roxy.cpp, which isn't linked into
# roxy_tests — `tests/e2e/test_cli.cpp` runs the built binary instead, so it
# needs the path and an ordering dependency. Without ROXY_CLI_PATH that file
# compiles to nothing, so a stripped-down configure still builds the suite.
target_compile_definitions(roxy_tests PRIVATE "ROXY_CLI_PATH=\"$<TARGET_FILE:roxy>\"")
add_dependencies(roxy_tests roxy)

# Fuzz targets (only when ENABLE_FUZZERS=ON — see the option block above and
# tests/fuzz/README.md). Each links its component's shared harness body plus the
# minimal library set, and the libFuzzer driver via -fsanitize=fuzzer.
if(ENABLE_FUZZERS)
    # Lexer only.
    add_executable(fuzz_lexer tests/fuzz/fuzz_lexer.cpp tests/fuzz/fuzz_one_lexer.cpp)
    target_link_libraries(fuzz_lexer roxy_shared roxy_core)
    target_link_options(fuzz_lexer PRIVATE -fsanitize=fuzzer)

    # Parser (pulls in the lexer + core).
    add_executable(fuzz_parser tests/fuzz/fuzz_parser.cpp tests/fuzz/fuzz_one_parser.cpp)
    target_link_libraries(fuzz_parser ${ROXY_LINK_START} roxy_compiler roxy_shared roxy_core ${ROXY_LINK_END})
    target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer)

    # LSP error-recovering parser (roxy_lsp pulls in vm + compiler).
    add_executable(fuzz_lsp_parser tests/fuzz/fuzz_lsp_parser.cpp tests/fuzz/fuzz_one_lsp_parser.cpp)
    target_link_libraries(fuzz_lsp_parser ${ROXY_LINK_START} roxy_lsp roxy_vm roxy_compiler roxy_shared roxy_core ${ROXY_LINK_END})
    target_link_options(fuzz_lsp_parser PRIVATE -fsanitize=fuzzer)
endif()

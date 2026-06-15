#pragma once

// Backend-parametric E2E test harness.
//
// Lets a single test body run on BOTH execution backends via doctest's
// TEST_CASE_TEMPLATE: the bytecode VM and the AOT C backend. The backend is a
// type parameter, so each TEST_CASE_TEMPLATE instantiation is registered as a
// separate doctest test case named `<TestName><VM>` / `<TestName><C>`.
//
// Usage:
//
//     #include "test_e2e_backend.hpp"
//
//     TEST_CASE_TEMPLATE("Return constant", Backend, RX_E2E_BACKENDS) {
//         auto result = Backend::run(R"(
//             fun main(): i32 { print(f"{42}"); return 0; }
//         )");
//         CHECK(result.success);
//         CHECK(result.stdout_output == "42\n");
//     }
//
// The unified result mirrors TestResult's field names (`success` / `value` /
// `stdout_output`) so converting an existing VM-only test is mechanical: swap
// `TEST_CASE` -> `TEST_CASE_TEMPLATE(..., Backend, RX_E2E_BACKENDS)`, swap
// `run_and_capture(src, "main")` -> `Backend::run(src)`, and leave the CHECKs.
//
// Backend selection at runtime is via doctest's name filter (the type tag is
// appended to the test name):
//     ./roxy_tests --test-case="*<VM>*"   # VM only  (sandbox-safe, fast)
//     ./roxy_tests --test-case="*<C>*"    # C only   (needs the system compiler)
//
// NOTE: `value` is the program result — the VM return value, or the process
// exit code on the C backend. Exit codes truncate to 0..255 (non-negative) on
// Unix, so prefer asserting on `stdout_output` for results outside that range.

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include <utility>

namespace rx {

// Unified result of running a Roxy program on either backend. Field names match
// TestResult so converted assertions need no edits.
struct E2EResult {
    bool success;           // compiled AND ran successfully
    i64 value;              // VM: return value of main(); C: process exit code (0..255)
    String stdout_output;   // captured stdout (identical across backends)
};

// Bytecode VM backend: compile to bytecode and run main() in-process.
struct VMBackend {
    static E2EResult run(const char* source, bool debug = false) {
        TestResult r = run_and_capture(source, "main", {}, debug);
        return E2EResult{r.success, r.value, std::move(r.stdout_output)};
    }
};

// AOT C backend: emit C++, compile with the system compiler, run the binary.
struct CBackend {
    static E2EResult run(const char* source, bool debug = false) {
        CBackendResult r = compile_and_run_cpp(source, debug);
        return E2EResult{r.compile_success && r.run_success,
                         static_cast<i64>(r.exit_code), std::move(r.stdout_output)};
    }
};

} // namespace rx

// The backend list expanded into TEST_CASE_TEMPLATE's type arguments.
#define RX_E2E_BACKENDS rx::VMBackend, rx::CBackend

// Friendly type tags so filters read `<VM>` / `<C>` instead of mangled names.
TYPE_TO_STRING_AS("VM", rx::VMBackend);
TYPE_TO_STRING_AS("C", rx::CBackend);

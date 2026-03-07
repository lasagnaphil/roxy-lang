#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/bytecode.hpp"

#include "roxy/core/string.hpp"

namespace rx {

struct IRModule;

// Result of running a test, includes return value and captured output
struct TestResult {
    i64 value;                    // Return value (always integer in Roxy)
    String stdout_output;         // Captured stdout
    bool success;                 // true if compilation and execution succeeded
};

// Result of compiling and running via C backend
struct CBackendResult {
    int exit_code;
    String stdout_output;
    bool compile_success;         // true if Roxy->C++ and C++->binary both succeeded
    bool run_success;             // true if binary executed successfully
};

// Helper to compile Roxy source to bytecode module
// Set debug=true to print generated IR for debugging
BCModule* compile(BumpAllocator& allocator, const char* source, bool debug = false);

// Helper to compile Roxy source to SSA IR (stops before bytecode lowering)
IRModule* compile_to_ir(BumpAllocator& allocator, const char* source, bool debug = false);

// Helper to compile Roxy source to C++ source string via CEmitter
String compile_to_cpp(const char* source, bool debug = false);

// Helper to compile Roxy source all the way through C backend and run the binary
CBackendResult compile_and_run_cpp(const char* source, bool debug = false);

// Helper to compile and run, returning result
// Set debug=true to print generated IR for debugging
Value compile_and_run(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

// Helper to compile and run with stdout/stderr capture
// Returns TestResult with value and captured output
TestResult run_and_capture(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

} // namespace rx

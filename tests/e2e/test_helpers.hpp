#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/bytecode.hpp"

#include <string>

namespace rx {

// Result of running a test, includes return value and captured output
struct TestResult {
    i64 value;                    // Return value (always integer in Roxy)
    std::string stdout_output;    // Captured stdout
    bool success;                 // true if compilation and execution succeeded
};

// Helper to compile Roxy source to bytecode module
// Set debug=true to print generated IR for debugging
BCModule* compile(BumpAllocator& allocator, const char* source, bool debug = false);

// Helper to compile and run, returning result
// Set debug=true to print generated IR for debugging
Value compile_and_run(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

// Helper to compile and run with stdout/stderr capture
// Returns TestResult with value and captured output
TestResult run_and_capture(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

} // namespace rx

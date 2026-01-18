#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/span.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/bytecode.hpp"

namespace rx {

// Helper to compile Roxy source to bytecode module
// Set debug=true to print generated IR for debugging
BCModule* compile(BumpAllocator& allocator, const char* source, bool debug = false);

// Helper to compile and run, returning result
// Set debug=true to print generated IR for debugging
Value compile_and_run(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

} // namespace rx

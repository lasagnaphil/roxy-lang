#pragma once

#include "roxy/vm/vm.hpp"

#include <cstdio>

namespace rx {

// Bytecode profiler. With ROXY_PROFILE_BYTECODE enabled at compile time, the
// dispatch loop accumulates per-opcode count and cycle totals. Without it,
// these become no-ops and there is zero hot-path overhead.
void bc_profile_reset();
void bc_profile_dump(FILE* out);

// Execute bytecode starting from the current call frame
// Returns true on success, false on error
// After return, result is in call_stack.back().registers[0]
// stop_depth: if > 0, stop when call stack reaches this depth (for nested interpretation)
bool interpret(RoxyVM* vm, u32 stop_depth = 0);

// Re-entrant call from native code into a bytecode function. Pushes a frame
// for `func_idx`, copies `argc` u64 args into the new frame's regs[0..argc),
// runs the interpreter until the frame returns, and returns its result.
//
// Used by Map's struct-key Hash/Eq dispatch to invoke user `hash()` / `eq()`
// methods from inside `map_hash_key` / `map_keys_equal`.
u64 call_user_function(RoxyVM* vm, u32 func_idx, const u64* args, u32 argc);

}

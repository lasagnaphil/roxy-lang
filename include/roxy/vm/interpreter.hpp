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

// CALL / CALL_INDIRECT do not clear a callee's non-parameter registers: the
// window is carved from the top of the shared register file, so in a release
// build it holds whatever an earlier call left there. Generated code must
// therefore never read a register before writing it on the same path — and
// that includes the unwinder, which reads the register a cleanup record names
// (lowering cuts records to blocks the value's definition can reach for
// exactly this reason). Debug builds fill the window with a poison pattern
// rather than zero, so a read of an unwritten register fails loudly in the
// test suite instead of passing there and misbehaving only in release.
// (vm_call poisons its frame the same way; the destructor and native-callback
// entry points memset theirs.)
inline constexpr u64 UNWRITTEN_REGISTER_POISON = 0xBAADF00DBAADF00Dull;

inline void poison_fresh_registers(u64* regs, u32 from, u32 to) {
#ifndef NDEBUG
    for (u32 i = from; i < to; i++) {
        regs[i] = UNWRITTEN_REGISTER_POISON;
    }
#else
    (void)regs;
    (void)from;
    (void)to;
#endif
}

} // namespace rx

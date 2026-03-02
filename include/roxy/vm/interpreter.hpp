#pragma once

#include "roxy/vm/vm.hpp"

namespace rx {

// Execute bytecode starting from the current call frame
// Returns true on success, false on error
// After return, result is in call_stack.back().registers[0]
// stop_depth: if > 0, stop when call stack reaches this depth (for nested interpretation)
bool interpret(RoxyVM* vm, u32 stop_depth = 0);

}

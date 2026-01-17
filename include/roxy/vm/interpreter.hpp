#pragma once

#include "roxy/vm/vm.hpp"

namespace rx {

// Execute bytecode starting from the current call frame
// Returns true on success, false on error
// After return, result is in call_stack.back().registers[0]
bool interpret(RoxyVM* vm);

}

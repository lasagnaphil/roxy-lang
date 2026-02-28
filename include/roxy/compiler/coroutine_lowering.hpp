#pragma once

#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/type_env.hpp"

namespace rx {

// Coroutine lowering pass
// Transforms coroutine IRFunctions (those with is_coroutine == true) into
// three replacement functions: init, resume, done.
//
// This pass runs after IR building and before IR validation.
// After this pass, no IROp::Yield instructions should remain.
void coroutine_lower(IRModule* module, BumpAllocator& allocator, TypeEnv& type_env);

}

#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Forward declarations
class NativeRegistry;

// Register all built-in native functions with the registry
void register_builtin_natives(NativeRegistry& registry);

// The name of the builtin module (auto-imported as prelude)
constexpr const char* BUILTIN_MODULE_NAME = "builtin";

}

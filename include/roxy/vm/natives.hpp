#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Forward declarations
class NativeRegistry;

// Register all built-in native functions with the registry
void register_builtin_natives(NativeRegistry& registry);

// Built-in native function names (for lookup during compilation)
constexpr const char* NATIVE_LIST_NEW = "list_new";
constexpr const char* NATIVE_LIST_LEN = "list_len";
constexpr const char* NATIVE_LIST_CAP = "list_cap";
constexpr const char* NATIVE_LIST_PUSH = "list_push";
constexpr const char* NATIVE_LIST_POP = "list_pop";
constexpr const char* NATIVE_PRINT = "print";
constexpr const char* NATIVE_PRINT_I64 = "print_i64";
constexpr const char* NATIVE_STR_CONCAT = "str_concat";
constexpr const char* NATIVE_STR_EQ = "str_eq";
constexpr const char* NATIVE_STR_NE = "str_ne";
constexpr const char* NATIVE_STR_LEN = "str_len";
constexpr const char* NATIVE_PRINT_STR = "print_str";

// to_string natives for primitive types (mangled as type$$to_string)
constexpr const char* NATIVE_BOOL_TO_STRING   = "bool$$to_string";
constexpr const char* NATIVE_I32_TO_STRING    = "i32$$to_string";
constexpr const char* NATIVE_I64_TO_STRING    = "i64$$to_string";
constexpr const char* NATIVE_F32_TO_STRING    = "f32$$to_string";
constexpr const char* NATIVE_F64_TO_STRING    = "f64$$to_string";
constexpr const char* NATIVE_STRING_TO_STRING = "string$$to_string";

// The name of the builtin module (auto-imported as prelude)
constexpr const char* BUILTIN_MODULE_NAME = "builtin";

}

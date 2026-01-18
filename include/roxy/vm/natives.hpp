#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/bytecode.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;
struct BCModule;

// Native function: array_new_int(size: i32) -> i32[]
// Creates a new integer array initialized to 0
void native_array_new_int(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: array_len(arr: T[]) -> i32
// Returns the length of an array
void native_array_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: print(value: i32)
// Prints an integer value followed by newline
void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Register all built-in native functions with the module
void register_builtin_natives(BCModule* module);

// Built-in native function names (for lookup during compilation)
constexpr const char* NATIVE_ARRAY_NEW_INT = "array_new_int";
constexpr const char* NATIVE_ARRAY_LEN = "array_len";
constexpr const char* NATIVE_PRINT = "print";

// Check if a function name is a built-in native function
bool is_builtin_native(const char* name, u32 len);

// Get the native function index for a built-in (-1 if not found)
i32 get_builtin_native_index(const char* name, u32 len);

}

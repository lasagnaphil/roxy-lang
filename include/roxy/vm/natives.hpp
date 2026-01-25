#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/bytecode.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;
class TypeCache;
class NativeRegistry;

// Native function: array_new_int(size: i32) -> i32[]
// Creates a new integer array initialized to 0
void native_array_new_int(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: array_len(arr: T[]) -> i32
// Returns the length of an array
void native_array_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: print(value: i32)
// Prints an integer value followed by newline
void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: str_concat(a: string, b: string) -> string
// Concatenates two strings and returns a new string
void native_str_concat(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: str_eq(a: string, b: string) -> bool
// Returns true if two strings are equal
void native_str_eq(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: str_ne(a: string, b: string) -> bool
// Returns true if two strings are not equal
void native_str_ne(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: str_len(s: string) -> i32
// Returns the length of a string
void native_str_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: print_str(s: string)
// Prints a string followed by newline
void native_print_str(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Register all built-in native functions with the registry
void register_builtin_natives(NativeRegistry& registry);

// Built-in native function names (for lookup during compilation)
constexpr const char* NATIVE_ARRAY_NEW_INT = "array_new_int";
constexpr const char* NATIVE_ARRAY_LEN = "array_len";
constexpr const char* NATIVE_PRINT = "print";
constexpr const char* NATIVE_STR_CONCAT = "str_concat";
constexpr const char* NATIVE_STR_EQ = "str_eq";
constexpr const char* NATIVE_STR_NE = "str_ne";
constexpr const char* NATIVE_STR_LEN = "str_len";
constexpr const char* NATIVE_PRINT_STR = "print_str";

// Check if a function name is a built-in native function
bool is_builtin_native(const char* name, u32 len);

// Get the native function index for a built-in (-1 if not found)
i32 get_builtin_native_index(const char* name, u32 len);

}

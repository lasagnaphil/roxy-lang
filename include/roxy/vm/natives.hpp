#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/bytecode.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;
class TypeCache;
class NativeRegistry;

// Native function: list_new(cap?: i32) -> List<T>
// Creates a new empty list with optional initial capacity
void native_list_new(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: list_len(lst: List<T>) -> i32
// Returns the length of a list
void native_list_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: list_cap(lst: List<T>) -> i32
// Returns the capacity of a list
void native_list_cap(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: list_push(lst: List<T>, val: T) -> void
// Pushes an element to the end of the list
void native_list_push(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: list_pop(lst: List<T>) -> T
// Pops and returns the last element of the list
void native_list_pop(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: print(value: i32)
// Prints an integer value followed by newline
void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Native function: print_i64(value: i64)
// Prints a 64-bit integer value followed by newline
void native_print_i64(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

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

// Check if a function name is a built-in native function
bool is_builtin_native(const char* name, u32 len);

// Get the native function index for a built-in (-1 if not found)
i32 get_builtin_native_index(const char* name, u32 len);

// The name of the builtin module (auto-imported as prelude)
constexpr const char* BUILTIN_MODULE_NAME = "builtin";

}

#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/array.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstring>

namespace rx {

// Native function: array_new_int(size: i32) -> i32[]
void native_array_new_int(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "array_new_int requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    i64 size = static_cast<i64>(regs[first_arg]);

    if (size < 0) {
        vm->error = "array size cannot be negative";
        return;
    }

    if (size > 1000000) {
        vm->error = "array size too large";
        return;
    }

    void* arr = array_alloc(vm, static_cast<u32>(size));
    if (!arr) {
        vm->error = "failed to allocate array";
        return;
    }

    // Initialize all elements to 0 (they're already null, set them to int 0)
    Value* elements = array_elements(arr);
    for (u32 i = 0; i < static_cast<u32>(size); i++) {
        elements[i] = Value::make_int(0);
    }

    regs[dst] = reinterpret_cast<u64>(arr);
}

// Native function: array_len(arr: T[]) -> i32
void native_array_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "array_len requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* arr_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (arr_ptr == nullptr) {
        vm->error = "array_len: null array reference";
        return;
    }

    u32 len = array_length(arr_ptr);
    regs[dst] = static_cast<u64>(len);
}

// Native function: print(value: i32)
// Note: With untyped registers, we interpret the value as i64 by default
void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    i64 val = static_cast<i64>(regs[first_arg]);

    // With untyped registers, we just print as integer
    printf("%lld\n", (long long)val);

    // print returns void, but we set dst to 0 for safety
    regs[dst] = 0;
}

// Native function: str_concat(a: string, b: string) -> string
void native_str_concat(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 2) {
        vm->error = "str_concat requires 2 arguments";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* str1 = reinterpret_cast<void*>(regs[first_arg]);
    void* str2 = reinterpret_cast<void*>(regs[first_arg + 1]);

    if (!str1 || !str2) {
        vm->error = "str_concat: null string argument";
        return;
    }

    void* result = string_concat(vm, str1, str2);
    if (!result) {
        vm->error = "str_concat: failed to allocate string";
        return;
    }

    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: str_eq(a: string, b: string) -> bool
void native_str_eq(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 2) {
        vm->error = "str_eq requires 2 arguments";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* str1 = reinterpret_cast<void*>(regs[first_arg]);
    void* str2 = reinterpret_cast<void*>(regs[first_arg + 1]);

    regs[dst] = string_equals(str1, str2) ? 1 : 0;
}

// Native function: str_ne(a: string, b: string) -> bool
void native_str_ne(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 2) {
        vm->error = "str_ne requires 2 arguments";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* str1 = reinterpret_cast<void*>(regs[first_arg]);
    void* str2 = reinterpret_cast<void*>(regs[first_arg + 1]);

    regs[dst] = string_equals(str1, str2) ? 0 : 1;
}

// Native function: str_len(s: string) -> i32
void native_str_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "str_len requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);

    if (!str) {
        vm->error = "str_len: null string";
        return;
    }

    regs[dst] = static_cast<u64>(string_length(str));
}

// Native function: print_str(s: string)
void native_print_str(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print_str requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);

    if (str) {
        printf("%s\n", string_chars(str));
    } else {
        printf("(null)\n");
    }

    regs[dst] = 0;
}

void register_builtin_natives(NativeRegistry& registry) {
    using TK = NativeTypeKind;

    // Register array_new_int(size: i32) -> i32[]
    // Use bind_native with type kinds for TypeCache-independent registration
    registry.bind_native(NATIVE_ARRAY_NEW_INT, native_array_new_int,
                         {TK::I32}, TK::ArrayI32);

    // Register array_len(arr: i32[]) -> i32
    registry.bind_native(NATIVE_ARRAY_LEN, native_array_len,
                         {TK::ArrayI32}, TK::I32);

    // Register print(value: i32) -> void
    registry.bind_native(NATIVE_PRINT, native_print,
                         {TK::I32}, TK::Void);

    // String functions
    // Register str_concat(a: string, b: string) -> string
    registry.bind_native(NATIVE_STR_CONCAT, native_str_concat,
                         {TK::String, TK::String}, TK::String);

    // Register str_eq(a: string, b: string) -> bool
    registry.bind_native(NATIVE_STR_EQ, native_str_eq,
                         {TK::String, TK::String}, TK::Bool);

    // Register str_ne(a: string, b: string) -> bool
    registry.bind_native(NATIVE_STR_NE, native_str_ne,
                         {TK::String, TK::String}, TK::Bool);

    // Register str_len(s: string) -> i32
    registry.bind_native(NATIVE_STR_LEN, native_str_len,
                         {TK::String}, TK::I32);

    // Register print_str(s: string) -> void
    registry.bind_native(NATIVE_PRINT_STR, native_print_str,
                         {TK::String}, TK::Void);
}

bool is_builtin_native(const char* name, u32 len) {
    if (len == strlen(NATIVE_ARRAY_NEW_INT) &&
        strncmp(name, NATIVE_ARRAY_NEW_INT, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_ARRAY_LEN) &&
        strncmp(name, NATIVE_ARRAY_LEN, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_PRINT) &&
        strncmp(name, NATIVE_PRINT, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_STR_CONCAT) &&
        strncmp(name, NATIVE_STR_CONCAT, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_STR_EQ) &&
        strncmp(name, NATIVE_STR_EQ, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_STR_NE) &&
        strncmp(name, NATIVE_STR_NE, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_STR_LEN) &&
        strncmp(name, NATIVE_STR_LEN, len) == 0) {
        return true;
    }
    if (len == strlen(NATIVE_PRINT_STR) &&
        strncmp(name, NATIVE_PRINT_STR, len) == 0) {
        return true;
    }
    return false;
}

i32 get_builtin_native_index(const char* name, u32 len) {
    // Native functions are registered in this order:
    // array_new_int (0), array_len (1), print (2),
    // str_concat (3), str_eq (4), str_ne (5), str_len (6), print_str (7)
    if (len == strlen(NATIVE_ARRAY_NEW_INT) &&
        strncmp(name, NATIVE_ARRAY_NEW_INT, len) == 0) {
        return 0;
    }
    if (len == strlen(NATIVE_ARRAY_LEN) &&
        strncmp(name, NATIVE_ARRAY_LEN, len) == 0) {
        return 1;
    }
    if (len == strlen(NATIVE_PRINT) &&
        strncmp(name, NATIVE_PRINT, len) == 0) {
        return 2;
    }
    if (len == strlen(NATIVE_STR_CONCAT) &&
        strncmp(name, NATIVE_STR_CONCAT, len) == 0) {
        return 3;
    }
    if (len == strlen(NATIVE_STR_EQ) &&
        strncmp(name, NATIVE_STR_EQ, len) == 0) {
        return 4;
    }
    if (len == strlen(NATIVE_STR_NE) &&
        strncmp(name, NATIVE_STR_NE, len) == 0) {
        return 5;
    }
    if (len == strlen(NATIVE_STR_LEN) &&
        strncmp(name, NATIVE_STR_LEN, len) == 0) {
        return 6;
    }
    if (len == strlen(NATIVE_PRINT_STR) &&
        strncmp(name, NATIVE_PRINT_STR, len) == 0) {
        return 7;
    }
    return -1;
}

}

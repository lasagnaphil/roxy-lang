#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstring>

namespace rx {

// Native function: list_new(cap?: i32) -> List<T>
static void native_list_new(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;

    u32 capacity = 0;
    if (argc >= 1) {
        i64 cap = static_cast<i64>(regs[first_arg]);
        if (cap < 0) {
            vm->error = "list capacity cannot be negative";
            return;
        }
        if (cap > 1000000) {
            vm->error = "list capacity too large";
            return;
        }
        capacity = static_cast<u32>(cap);
    }

    void* lst = list_alloc(vm, capacity);
    if (!lst) {
        vm->error = "failed to allocate list";
        return;
    }

    regs[dst] = reinterpret_cast<u64>(lst);
}

// Native function: list_len(lst: List<T>) -> i32
static void native_list_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "list_len requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_len: null list reference";
        return;
    }

    u32 len = list_length(lst_ptr);
    regs[dst] = static_cast<u64>(len);
}

// Native function: list_cap(lst: List<T>) -> i32
static void native_list_cap(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "list_cap requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_cap: null list reference";
        return;
    }

    u32 cap = list_capacity(lst_ptr);
    regs[dst] = static_cast<u64>(cap);
}

// Native function: list_push(lst: List<T>, val: T) -> void
static void native_list_push(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 2) {
        vm->error = "list_push requires 2 arguments";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_push: null list reference";
        return;
    }

    Value val = Value::from_u64(regs[first_arg + 1]);
    list_push(lst_ptr, val);
    regs[dst] = 0;
}

// Native function: list_pop(lst: List<T>) -> T
static void native_list_pop(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "list_pop requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_pop: null list reference";
        return;
    }

    Value result = list_pop(lst_ptr);
    regs[dst] = result.as_u64();
}

// Native function: print(value: i32)
// Note: With untyped registers, we interpret the value as i64 by default
static void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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

// Native function: print_i64(value: i64)
static void native_print_i64(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print_i64 requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack.back().registers;
    i64 val = static_cast<i64>(regs[first_arg]);

    printf("%lld\n", (long long)val);
    regs[dst] = 0;
}

// Native function: str_concat(a: string, b: string) -> string
static void native_str_concat(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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
static void native_str_eq(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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
static void native_str_ne(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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
static void native_str_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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
static void native_print_str(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
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

// Native function: bool$$to_string(val: bool) -> string
static void native_bool_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    bool val = regs[first_arg] != 0;
    const char* s = val ? "true" : "false";
    u32 len = val ? 4 : 5;
    void* result = string_alloc(vm, s, len);
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: i32$$to_string(val: i32) -> string
static void native_i32_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    i32 val = static_cast<i32>(regs[first_arg]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: i64$$to_string(val: i64) -> string
static void native_i64_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    i64 val = static_cast<i64>(regs[first_arg]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: f32$$to_string(val: f32) -> string
static void native_f32_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    f32 val;
    memcpy(&val, &regs[first_arg], sizeof(f32));
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", (double)val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: f64$$to_string(val: f64) -> string
static void native_f64_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    f64 val;
    memcpy(&val, &regs[first_arg], sizeof(f64));
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: string$$to_string(val: string) -> string (identity)
static void native_string_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = regs[first_arg];
}

void register_builtin_natives(NativeRegistry& registry) {
    using TK = NativeTypeKind;

    // List natives - registered with I64 type kinds since list pointers and values
    // are 64-bit values. The semantic analyzer handles actual type checking.
    registry.bind_native(NATIVE_LIST_NEW, native_list_new,
                         {TK::I32}, TK::I64);

    registry.bind_native(NATIVE_LIST_LEN, native_list_len,
                         {TK::I64}, TK::I32);

    registry.bind_native(NATIVE_LIST_CAP, native_list_cap,
                         {TK::I64}, TK::I32);

    registry.bind_native(NATIVE_LIST_PUSH, native_list_push,
                         {TK::I64, TK::I64}, TK::Void);

    registry.bind_native(NATIVE_LIST_POP, native_list_pop,
                         {TK::I64}, TK::I64);

    // Register print(value: i32) -> void
    registry.bind_native(NATIVE_PRINT, native_print,
                         {TK::I32}, TK::Void);

    // Register print_i64(value: i64) -> void
    registry.bind_native(NATIVE_PRINT_I64, native_print_i64,
                         {TK::I64}, TK::Void);

    // String functions
    registry.bind_native(NATIVE_STR_CONCAT, native_str_concat,
                         {TK::String, TK::String}, TK::String);

    registry.bind_native(NATIVE_STR_EQ, native_str_eq,
                         {TK::String, TK::String}, TK::Bool);

    registry.bind_native(NATIVE_STR_NE, native_str_ne,
                         {TK::String, TK::String}, TK::Bool);

    registry.bind_native(NATIVE_STR_LEN, native_str_len,
                         {TK::String}, TK::I32);

    registry.bind_native(NATIVE_PRINT_STR, native_print_str,
                         {TK::String}, TK::Void);

    // to_string natives for primitive types
    registry.bind_native(NATIVE_BOOL_TO_STRING, native_bool_to_string,
                         {TK::Bool}, TK::String);
    registry.bind_native(NATIVE_I32_TO_STRING, native_i32_to_string,
                         {TK::I32}, TK::String);
    registry.bind_native(NATIVE_I64_TO_STRING, native_i64_to_string,
                         {TK::I64}, TK::String);
    registry.bind_native(NATIVE_F32_TO_STRING, native_f32_to_string,
                         {TK::F32}, TK::String);
    registry.bind_native(NATIVE_F64_TO_STRING, native_f64_to_string,
                         {TK::F64}, TK::String);
    registry.bind_native(NATIVE_STRING_TO_STRING, native_string_to_string,
                         {TK::String}, TK::String);
}

}

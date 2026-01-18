#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/array.hpp"
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
    return false;
}

i32 get_builtin_native_index(const char* name, u32 len) {
    // Native functions are registered in this order: array_new_int (0), array_len (1), print (2)
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
    return -1;
}

}

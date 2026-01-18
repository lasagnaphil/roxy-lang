#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/array.hpp"
#include "roxy/vm/value.hpp"

#include <cstdio>
#include <cstring>

namespace rx {

// Native function: array_new_int(size: i32) -> i32[]
void native_array_new_int(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "array_new_int requires 1 argument";
        return;
    }

    Value* regs = vm->call_stack.back().registers;
    i64 size = regs[first_arg].as_int;

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

    regs[dst] = Value::make_ptr(arr);
}

// Native function: array_len(arr: T[]) -> i32
void native_array_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "array_len requires 1 argument";
        return;
    }

    Value* regs = vm->call_stack.back().registers;
    Value arr_val = regs[first_arg];

    if (!arr_val.is_ptr() || arr_val.as_ptr == nullptr) {
        vm->error = "array_len: null array reference";
        return;
    }

    u32 len = array_length(arr_val.as_ptr);
    regs[dst] = Value::make_int(static_cast<i64>(len));
}

// Native function: print(value: i32)
void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print requires 1 argument";
        return;
    }

    Value* regs = vm->call_stack.back().registers;
    Value val = regs[first_arg];

    switch (val.type) {
        case Value::Type::Null:
            printf("null\n");
            break;
        case Value::Type::Bool:
            printf("%s\n", val.as_bool ? "true" : "false");
            break;
        case Value::Type::Int:
            printf("%lld\n", (long long)val.as_int);
            break;
        case Value::Type::Float:
            printf("%g\n", val.as_float);
            break;
        case Value::Type::Ptr:
            printf("<ptr %p>\n", val.as_ptr);
            break;
        case Value::Type::Weak:
            printf("<weak %p>\n", val.as_weak.ptr);
            break;
    }

    // print returns void, but we set dst to null for safety
    regs[dst] = Value::make_null();
}

void register_builtin_natives(BCModule* module) {
    // Register array_new_int
    BCNativeFunction array_new_int_fn;
    array_new_int_fn.name = StringView(NATIVE_ARRAY_NEW_INT, strlen(NATIVE_ARRAY_NEW_INT));
    array_new_int_fn.func = native_array_new_int;
    array_new_int_fn.param_count = 1;
    module->native_functions.push_back(array_new_int_fn);

    // Register array_len
    BCNativeFunction array_len_fn;
    array_len_fn.name = StringView(NATIVE_ARRAY_LEN, strlen(NATIVE_ARRAY_LEN));
    array_len_fn.func = native_array_len;
    array_len_fn.param_count = 1;
    module->native_functions.push_back(array_len_fn);

    // Register print
    BCNativeFunction print_fn;
    print_fn.name = StringView(NATIVE_PRINT, strlen(NATIVE_PRINT));
    print_fn.func = native_print;
    print_fn.param_count = 1;
    module->native_functions.push_back(print_fn);
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

#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rx {

// Allocates an empty list (capacity 0). Non-method, no self.
static void native_list_alloc(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* lst = list_alloc(vm, 0);
    if (!lst) {
        vm->error = "failed to allocate list";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(lst);
}

// Constructor method. Receives self + optional capacity.
static void native_list_init(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]); // self
    if (!lst_ptr) {
        vm->error = "list init: null self";
        return;
    }

    if (argc >= 2) { // self + capacity
        i64 cap = static_cast<i64>(regs[first_arg + 1]);
        if (cap < 0) {
            vm->error = "list capacity cannot be negative";
            return;
        }
        if (cap > 1000000) {
            vm->error = "list capacity too large";
            return;
        }
        u32 capacity = static_cast<u32>(cap);
        if (capacity > 0) {
            ListHeader* header = get_list_header(lst_ptr);
            header->capacity = capacity;
            header->elements = static_cast<Value*>(malloc(sizeof(Value) * capacity));
            memset(header->elements, 0, sizeof(Value) * capacity);
        }
    }
    regs[dst] = 0; // void
}

// Destructor method. Receives self, frees element buffer.
static void native_list_delete(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]); // self
    if (lst_ptr) {
        ListHeader* header = get_list_header(lst_ptr);
        free(header->elements);
        header->elements = nullptr;
        header->length = 0;
        header->capacity = 0;
    }
    regs[dst] = 0;
}

// Native function: list_copy(src: List<T>) -> List<T> (deep copy)
static void native_list_copy(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* src = reinterpret_cast<void*>(regs[first_arg]);
    if (!src) {
        vm->error = "list_copy: null source";
        return;
    }
    void* copy = list_copy(vm, src);
    if (!copy) {
        vm->error = "list_copy: allocation failed";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(copy);
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

// Native function: print(s: string)
static void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print requires 1 argument";
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

    // List<T> - registered as a generic native type
    registry.register_generic_type("List", 1, "list_alloc", native_list_alloc);
    registry.bind_generic_constructor("List", native_list_init,
                                      0, {concrete_param(TK::I32)});
    registry.bind_generic_destructor("List", native_list_delete);
    registry.bind_generic_copy_constructor("List", "list_copy", native_list_copy);
    registry.bind_generic_method("List", "len",  native_list_len,
                                 {}, concrete_param(TK::I32));
    registry.bind_generic_method("List", "cap",  native_list_cap,
                                 {}, concrete_param(TK::I32));
    registry.bind_generic_method("List", "push", native_list_push,
                                 {type_param(0)}, concrete_param(TK::Void));
    registry.bind_generic_method("List", "pop",  native_list_pop,
                                 {}, type_param(0));

    // Register print(s: string) -> void
    registry.bind_native("print", native_print,
                         {TK::String}, TK::Void);

    // String functions
    registry.bind_native("str_concat", native_str_concat,
                         {TK::String, TK::String}, TK::String);

    registry.bind_native("str_eq", native_str_eq,
                         {TK::String, TK::String}, TK::Bool);

    registry.bind_native("str_ne", native_str_ne,
                         {TK::String, TK::String}, TK::Bool);

    registry.bind_native("str_len", native_str_len,
                         {TK::String}, TK::I32);

    // to_string natives for primitive types
    registry.bind_native("bool$$to_string", native_bool_to_string,
                         {TK::Bool}, TK::String);
    registry.bind_native("i32$$to_string", native_i32_to_string,
                         {TK::I32}, TK::String);
    registry.bind_native("i64$$to_string", native_i64_to_string,
                         {TK::I64}, TK::String);
    registry.bind_native("f32$$to_string", native_f32_to_string,
                         {TK::F32}, TK::String);
    registry.bind_native("f64$$to_string", native_f64_to_string,
                         {TK::F64}, TK::String);
    registry.bind_native("string$$to_string", native_string_to_string,
                         {TK::String}, TK::String);
}

}

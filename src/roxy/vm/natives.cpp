#include "roxy/vm/natives.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/map.hpp"
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

// ===== Hash native functions =====

// SplitMix64 bit mixer
static u64 splitmix64(u64 x) {
    x ^= x >> 30;
    x *= 0xbf58476d1ce4e5b9ULL;
    x ^= x >> 27;
    x *= 0x94d049bb133111ebULL;
    x ^= x >> 31;
    return x;
}

static void native_bool_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<i64>(regs[first_arg] != 0 ? 1 : 0);
}

static void native_i8_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_i16_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_i32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_i64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_u8_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_u16_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_u32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_u64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(regs[first_arg])));
}

static void native_f32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    f32 val;
    memcpy(&val, &regs[first_arg], sizeof(f32));
    if (val == 0.0f) val = 0.0f; // Normalize -0
    u32 bits;
    memcpy(&bits, &val, sizeof(u32));
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(static_cast<u64>(bits))));
}

static void native_f64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    f64 val;
    memcpy(&val, &regs[first_arg], sizeof(f64));
    if (val == 0.0) val = 0.0; // Normalize -0
    u64 bits;
    memcpy(&bits, &val, sizeof(u64));
    regs[dst] = static_cast<u64>(static_cast<i64>(splitmix64(bits)));
}

static void native_string_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);
    if (!str) {
        regs[dst] = 0;
        return;
    }
    // FNV-1a
    u64 h = 14695981039346656037ULL;
    const char* data = string_chars(str);
    u32 len = string_length(str);
    for (u32 i = 0; i < len; i++) {
        h ^= static_cast<u64>(static_cast<u8>(data[i]));
        h *= 1099511628211ULL;
    }
    regs[dst] = static_cast<u64>(static_cast<i64>(h));
}

// ===== List index native functions =====

// Native function: list index (get element by index)
static void native_list_index(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!lst_ptr) {
        vm->error = "list index: null list reference";
        return;
    }
    i64 index = static_cast<i64>(regs[first_arg + 1]);
    Value result;
    if (!list_get(lst_ptr, index, result, &vm->error)) {
        return;
    }
    regs[dst] = result.as_u64();
}

// Native function: list index_mut (set element by index)
static void native_list_index_mut(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!lst_ptr) {
        vm->error = "list index_mut: null list reference";
        return;
    }
    i64 index = static_cast<i64>(regs[first_arg + 1]);
    Value value = Value::from_u64(regs[first_arg + 2]);
    if (!list_set(lst_ptr, index, value, &vm->error)) {
        return;
    }
    regs[dst] = 0;
}

// ===== Map native functions =====

// Determine MapKeyKind from type kind constant passed as i32
static MapKeyKind key_kind_from_i32(i32 val) {
    return static_cast<MapKeyKind>(static_cast<u8>(val));
}

static void native_map_alloc(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    // Allocate with Integer key kind by default; constructor sets the real key_kind
    void* map = map_alloc(vm, MapKeyKind::Integer, 0);
    if (!map) {
        vm->error = "failed to allocate map";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(map);
}

// Constructor: receives self, key_kind, optional capacity
static void native_map_init(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]); // self
    if (!map_ptr) {
        vm->error = "map init: null self";
        return;
    }

    MapHeader* header = get_map_header(map_ptr);

    // First arg after self is key_kind
    if (argc >= 2) {
        i32 kind_val = static_cast<i32>(regs[first_arg + 1]);
        header->key_kind = key_kind_from_i32(kind_val);
    }

    // Second arg after self is optional capacity
    if (argc >= 3) {
        i64 cap = static_cast<i64>(regs[first_arg + 2]);
        if (cap < 0) {
            vm->error = "map capacity cannot be negative";
            return;
        }
        if (cap > 1000000) {
            vm->error = "map capacity too large";
            return;
        }
        if (cap > 0) {
            u32 actual = 8;
            while (actual < static_cast<u32>(cap)) actual *= 2;
            // Allocate buckets
            header->capacity = actual;
            header->distances = static_cast<u8*>(calloc(actual, sizeof(u8)));
            header->keys = static_cast<u64*>(calloc(actual, sizeof(u64)));
            header->values = static_cast<u64*>(calloc(actual, sizeof(u64)));
        }
    }
    regs[dst] = 0;
}

static void native_map_delete(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (map_ptr) {
        MapHeader* header = get_map_header(map_ptr);
        free(header->distances);
        free(header->keys);
        free(header->values);
        header->distances = nullptr;
        header->keys = nullptr;
        header->values = nullptr;
        header->length = 0;
        header->capacity = 0;
    }
    regs[dst] = 0;
}

static void native_map_copy(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* src = reinterpret_cast<void*>(regs[first_arg]);
    if (!src) {
        vm->error = "map_copy: null source";
        return;
    }
    void* copy = map_copy(vm, src);
    if (!copy) {
        vm->error = "map_copy: allocation failed";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(copy);
}

static void native_map_len(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_len: null map reference";
        return;
    }
    regs[dst] = static_cast<u64>(map_length(map_ptr));
}

static void native_map_contains(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_contains: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    regs[dst] = map_contains(map_ptr, key) ? 1 : 0;
}

static void native_map_get(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_get: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    u64 value;
    if (!map_get(map_ptr, key, value, &vm->error)) {
        return;
    }
    regs[dst] = value;
}

static void native_map_insert(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_insert: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    u64 value = regs[first_arg + 2];
    map_insert(map_ptr, key, value);
    regs[dst] = 0;
}

static void native_map_remove(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_remove: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    regs[dst] = map_remove(map_ptr, key) ? 1 : 0;
}

static void native_map_clear(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_clear: null map reference";
        return;
    }
    map_clear(map_ptr);
    regs[dst] = 0;
}

static void native_map_keys(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_keys: null map reference";
        return;
    }
    void* keys_list = map_keys(vm, map_ptr);
    if (!keys_list) {
        vm->error = "map_keys: allocation failed";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(keys_list);
}

static void native_map_values(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_values: null map reference";
        return;
    }
    void* values_list = map_values(vm, map_ptr);
    if (!values_list) {
        vm->error = "map_values: allocation failed";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(values_list);
}

// Native function: map index (get value by key)
static void native_map_index(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map index: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    u64 value;
    if (!map_get(map_ptr, key, value, &vm->error)) {
        return;
    }
    regs[dst] = value;
}

// Native function: map index_mut (insert/update key-value pair)
static void native_map_index_mut(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack.back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map index_mut: null map reference";
        return;
    }
    u64 key = regs[first_arg + 1];
    u64 value = regs[first_arg + 2];
    map_insert(map_ptr, key, value);
    regs[dst] = 0;
}

void register_builtin_natives(NativeRegistry& registry) {
    // List<T> - registered as a generic native type
    registry.register_generic_type("List<T>", "list_alloc", native_list_alloc);
    registry.bind_constructor(native_list_init, "fun List<T>.new(cap: i32)", 0);
    registry.bind_generic_destructor("List", native_list_delete);
    registry.bind_generic_copy_constructor("List", "list_copy", native_list_copy);
    registry.bind_method(native_list_len,       "fun List<T>.len(): i32");
    registry.bind_method(native_list_cap,       "fun List<T>.cap(): i32");
    registry.bind_method(native_list_push,      "fun List<T>.push(val: T)");
    registry.bind_method(native_list_pop,       "fun List<T>.pop(): T");
    registry.bind_method(native_list_index,     "fun List<T>.index(idx: i32): T");
    registry.bind_method(native_list_index_mut, "fun List<T>.index_mut(idx: i32, val: T)");

    // Free functions
    registry.bind_native(native_print, "fun print(s: string)");

    // String functions
    registry.bind_native(native_str_concat, "fun str_concat(a: string, b: string): string");
    registry.bind_native(native_str_eq, "fun str_eq(a: string, b: string): bool");
    registry.bind_native(native_str_ne, "fun str_ne(a: string, b: string): bool");
    registry.bind_native(native_str_len, "fun str_len(s: string): i32");

    // to_string natives for primitive types ($$-mangled name override)
    registry.bind_native("bool$$to_string",   native_bool_to_string,   "fun to_string(val: bool): string");
    registry.bind_native("i32$$to_string",    native_i32_to_string,    "fun to_string(val: i32): string");
    registry.bind_native("i64$$to_string",    native_i64_to_string,    "fun to_string(val: i64): string");
    registry.bind_native("f32$$to_string",    native_f32_to_string,    "fun to_string(val: f32): string");
    registry.bind_native("f64$$to_string",    native_f64_to_string,    "fun to_string(val: f64): string");
    registry.bind_native("string$$to_string", native_string_to_string, "fun to_string(val: string): string");

    // Hash natives for primitive types
    registry.bind_native("bool$$hash",   native_bool_hash,   "fun hash(val: bool): i64");
    registry.bind_native("i8$$hash",     native_i8_hash,     "fun hash(val: i8): i64");
    registry.bind_native("i16$$hash",    native_i16_hash,    "fun hash(val: i16): i64");
    registry.bind_native("i32$$hash",    native_i32_hash,    "fun hash(val: i32): i64");
    registry.bind_native("i64$$hash",    native_i64_hash,    "fun hash(val: i64): i64");
    registry.bind_native("u8$$hash",     native_u8_hash,     "fun hash(val: u8): i64");
    registry.bind_native("u16$$hash",    native_u16_hash,    "fun hash(val: u16): i64");
    registry.bind_native("u32$$hash",    native_u32_hash,    "fun hash(val: u32): i64");
    registry.bind_native("u64$$hash",    native_u64_hash,    "fun hash(val: u64): i64");
    registry.bind_native("f32$$hash",    native_f32_hash,    "fun hash(val: f32): i64");
    registry.bind_native("f64$$hash",    native_f64_hash,    "fun hash(val: f64): i64");
    registry.bind_native("string$$hash", native_string_hash, "fun hash(val: string): i64");

    // Map<K, V> - registered as a generic native type with 2 type params
    registry.register_generic_type("Map<K, V>", "map_alloc", native_map_alloc);
    // Constructor receives: key_kind (i32, hidden), capacity (i32, optional)
    registry.bind_constructor(native_map_init, "fun Map<K, V>.new(key_kind: i32, capacity: i32)", 1);
    registry.bind_generic_destructor("Map", native_map_delete);
    registry.bind_generic_copy_constructor("Map", "map_copy", native_map_copy);
    registry.bind_method(native_map_len,       "fun Map<K, V>.len(): i32");
    registry.bind_method(native_map_contains,  "fun Map<K, V>.contains(key: K): bool");
    registry.bind_method(native_map_get,       "fun Map<K, V>.get(key: K): V");
    registry.bind_method(native_map_insert,    "fun Map<K, V>.insert(key: K, val: V)");
    registry.bind_method(native_map_remove,    "fun Map<K, V>.remove(key: K): bool");
    registry.bind_method(native_map_clear,     "fun Map<K, V>.clear()");
    registry.bind_method(native_map_keys,      "fun Map<K, V>.keys(): List<K>");
    registry.bind_method(native_map_values,    "fun Map<K, V>.values(): List<V>");
    registry.bind_method(native_map_index,     "fun Map<K, V>.index(key: K): V");
    registry.bind_method(native_map_index_mut, "fun Map<K, V>.index_mut(key: K, val: V)");
}

}

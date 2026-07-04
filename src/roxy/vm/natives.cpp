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
#include <cmath>
#include <chrono>

namespace rx {

static constexpr i64 MAX_COLLECTION_CAPACITY = 1000000;

// Allocates an empty list (capacity 0). Non-method, no self.
// argc >= 1: first arg is element_slot_count
// argc >= 2: second arg is element_is_inline (0 = false, nonzero = true)
static void native_list_alloc(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    u32 element_slot_count = (argc >= 1) ? static_cast<u32>(regs[first_arg]) : 2;
    bool element_is_inline = (argc >= 2) ? (regs[first_arg + 1] != 0) : true;
    void* lst = list_alloc(vm, 0, element_slot_count, element_is_inline);
    if (!lst) {
        vm->error = "failed to allocate list";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(lst);
}

// Constructor method. Receives self + optional capacity.
static void native_list_init(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
        if (cap > MAX_COLLECTION_CAPACITY) {
            vm->error = "list capacity too large (max 1000000)";
            return;
        }
        u32 capacity = static_cast<u32>(cap);
        if (capacity > 0) {
            ListHeader* header = get_list_header(lst_ptr);
            u32 esc = header->element_slot_count;
            header->capacity = capacity;
            header->elements = static_cast<u32*>(malloc(sizeof(u32) * esc * capacity));
            memset(header->elements, 0, sizeof(u32) * esc * capacity);
        }
    }
    regs[dst] = 0; // void
}

// Destructor method. Receives self, frees element buffer.
static void native_list_delete(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_push: null list reference";
        return;
    }

    ListHeader* header = get_list_header(lst_ptr);
    if (header->element_is_inline) {
        // Primitive: value is directly in register
        list_push_slots(lst_ptr, reinterpret_cast<const u32*>(&regs[first_arg + 1]));
    } else {
        // Struct: register holds pointer to struct data
        u32* src = reinterpret_cast<u32*>(regs[first_arg + 1]);
        list_push_slots(lst_ptr, src);
    }
    regs[dst] = 0;
}

// Native function: list_pop(lst: List<T>) -> T
static void native_list_pop(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "list_pop requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack_back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);

    if (lst_ptr == nullptr) {
        vm->error = "list_pop: null list reference";
        return;
    }

    ListHeader* header = get_list_header(lst_ptr);
    u32* ptr = list_pop_ptr(lst_ptr);
    if (!ptr) {
        regs[dst] = 0;
        return;
    }
    if (header->element_is_inline) {
        u64 val = 0;
        memcpy(&val, ptr, sizeof(u32) * header->element_slot_count);
        regs[dst] = val;
    } else {
        // Struct: return pointer to popped element data
        regs[dst] = reinterpret_cast<u64>(ptr);
    }
}

// Native function: print(s: string)
static void native_print(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    if (argc < 1) {
        vm->error = "print requires 1 argument";
        return;
    }

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
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

    u64* regs = vm->call_stack_back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);

    if (!str) {
        vm->error = "str_len: null string";
        return;
    }

    regs[dst] = static_cast<u64>(string_length(str));
}



// Native function: bool$$to_string(val: bool) -> string
static void native_bool_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    bool val = regs[first_arg] != 0;
    const char* s = val ? "true" : "false";
    u32 len = val ? 4 : 5;
    void* result = string_alloc(vm, s, len);
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: i32$$to_string(val: i32) -> string
static void native_i32_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    i32 val = static_cast<i32>(regs[first_arg]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%d", val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: i64$$to_string(val: i64) -> string
static void native_i64_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    i64 val = static_cast<i64>(regs[first_arg]);
    char buf[32];
    int len = snprintf(buf, sizeof(buf), "%lld", (long long)val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: f32$$to_string(val: f32) -> string
static void native_f32_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    f32 val;
    memcpy(&val, &regs[first_arg], sizeof(f32));
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", (double)val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: f64$$to_string(val: f64) -> string
static void native_f64_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    f64 val;
    memcpy(&val, &regs[first_arg], sizeof(f64));
    char buf[64];
    int len = snprintf(buf, sizeof(buf), "%g", val);
    void* result = string_alloc(vm, buf, static_cast<u32>(len));
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: string$$to_string(val: string) -> string (identity)
static void native_string_to_string(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = regs[first_arg] != 0 ? 1u : 0u;
}

static void native_i8_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_i16_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_i32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_i64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_u8_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_u16_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_u32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_u64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    regs[dst] = splitmix64(regs[first_arg]);
}

static void native_f32_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    f32 val;
    memcpy(&val, &regs[first_arg], sizeof(f32));
    if (val == 0.0f) val = 0.0f; // Normalize -0
    u32 bits;
    memcpy(&bits, &val, sizeof(u32));
    regs[dst] = splitmix64(static_cast<u64>(bits));
}

static void native_f64_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    f64 val;
    memcpy(&val, &regs[first_arg], sizeof(f64));
    if (val == 0.0) val = 0.0; // Normalize -0
    u64 bits;
    memcpy(&bits, &val, sizeof(u64));
    regs[dst] = splitmix64(bits);
}

static void native_string_hash(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
    regs[dst] = h;
}

// ===== List index native functions =====

// Native function: list index (get element by index)
static void native_list_index(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!lst_ptr) {
        vm->error = "list index: null list reference";
        return;
    }
    i64 index = static_cast<i64>(regs[first_arg + 1]);
    u32* ptr = list_get_ptr(lst_ptr, index, &vm->error);
    if (!ptr) return;
    ListHeader* header = get_list_header(lst_ptr);
    if (header->element_is_inline) {
        u64 val = 0;
        memcpy(&val, ptr, sizeof(u32) * header->element_slot_count);
        regs[dst] = val;
    } else {
        regs[dst] = reinterpret_cast<u64>(ptr);
    }
}

// Native function: list index_mut (set element by index)
static void native_list_index_mut(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* lst_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!lst_ptr) {
        vm->error = "list index_mut: null list reference";
        return;
    }
    i64 index = static_cast<i64>(regs[first_arg + 1]);
    ListHeader* header = get_list_header(lst_ptr);
    if (header->element_is_inline) {
        if (!list_set_slots(lst_ptr, index, reinterpret_cast<const u32*>(&regs[first_arg + 2]), &vm->error)) {
            return;
        }
    } else {
        u32* src = reinterpret_cast<u32*>(regs[first_arg + 2]);
        if (!list_set_slots(lst_ptr, index, src, &vm->error)) {
            return;
        }
    }
    regs[dst] = 0;
}

// ===== Map native functions =====

// Determine MapKeyKind from type kind constant passed as i32
static MapKeyKind key_kind_from_i32(i32 val) {
    return static_cast<MapKeyKind>(static_cast<u8>(val));
}

// IR-builder-emitted args (in order, all i32 in registers):
//   0: key_slot_count (u32 slots per key)
//   1: key_is_inline (0 = struct-by-pointer, nonzero = primitive-by-value)
//   2: value_slot_count
//   3: value_is_inline
//   4: hash_fn_index (i32; -1 = no custom Hash, runtime falls back to bytewise)
//   5: eq_fn_index   (i32; -1 = no custom Eq)
// Fallback: missing args use legacy defaults (2 slots, inline, no custom hash/eq).
static void native_map_alloc(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    u8 key_slot_count = 2;
    bool key_is_inline = true;
    u8 value_slot_count = 2;
    bool value_is_inline = true;
    u32 hash_fn_index = UINT32_MAX;
    u32 eq_fn_index = UINT32_MAX;
    if (argc >= 1) {
        i32 ksc = static_cast<i32>(regs[first_arg]);
        if (ksc > 0 && ksc <= 255) key_slot_count = static_cast<u8>(ksc);
    }
    if (argc >= 2) {
        key_is_inline = (regs[first_arg + 1] != 0);
    }
    if (argc >= 3) {
        i32 vsc = static_cast<i32>(regs[first_arg + 2]);
        if (vsc > 0 && vsc <= 255) value_slot_count = static_cast<u8>(vsc);
    }
    if (argc >= 4) {
        value_is_inline = (regs[first_arg + 3] != 0);
    }
    if (argc >= 5) {
        i32 idx = static_cast<i32>(regs[first_arg + 4]);
        if (idx >= 0) hash_fn_index = static_cast<u32>(idx);
    }
    if (argc >= 6) {
        i32 idx = static_cast<i32>(regs[first_arg + 5]);
        if (idx >= 0) eq_fn_index = static_cast<u32>(idx);
    }
    // Allocate with Integer key kind by default; constructor sets the real key_kind
    void* map = map_alloc(vm, MapKeyKind::Integer, 0,
                          key_slot_count, key_is_inline,
                          value_slot_count, value_is_inline,
                          hash_fn_index, eq_fn_index);
    if (!map) {
        vm->error = "failed to allocate map";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(map);
}

// Constructor: receives self, key_kind, optional capacity
static void native_map_init(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]); // self
    if (!map_ptr) {
        vm->error = "map init: null self";
        return;
    }

    MapHeader* header = get_map_header(map_ptr);

    // First arg after self is key_kind
    if (argc >= 2) {
        i32 kind_val = static_cast<i32>(regs[first_arg + 1]);
        header->key_kind = static_cast<u8>(key_kind_from_i32(kind_val));
    }

    // Second arg after self is optional capacity
    if (argc >= 3) {
        i64 cap = static_cast<i64>(regs[first_arg + 2]);
        if (cap < 0) {
            vm->error = "map capacity cannot be negative";
            return;
        }
        if (cap > MAX_COLLECTION_CAPACITY) {
            vm->error = "map capacity too large (max 1000000)";
            return;
        }
        if (cap > 0) {
            u32 actual = 8;
            while (actual < static_cast<u32>(cap)) actual *= 2;
            // Allocate buckets using the header's key/value slot counts that
            // were set during native_map_alloc (via IR-builder args). Only
            // commit them to the header if all three succeed, so an OOM failure
            // doesn't leave a non-zero capacity with null bucket arrays.
            u8* distances = static_cast<u8*>(calloc(actual, sizeof(u8)));
            u32* keys = static_cast<u32*>(calloc(
                static_cast<size_t>(actual) * header->key_slot_count, sizeof(u32)));
            u32* values = static_cast<u32*>(calloc(
                static_cast<size_t>(actual) * header->value_slot_count, sizeof(u32)));
            if (!distances || !keys || !values) {
                free(distances);
                free(keys);
                free(values);
                vm->error = "map reserve: allocation failed";
                return;
            }
            header->capacity = actual;
            header->distances = distances;
            header->keys = keys;
            header->values = values;
        }
    }
    regs[dst] = 0;
}

static void native_map_delete(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_len: null map reference";
        return;
    }
    regs[dst] = static_cast<u64>(map_length(map_ptr));
}

// Read the source pointer for a key from the argument register at offset
// `first_arg + 1`. Inline keys (primitives ≤ 8B packed into the u64 register)
// point at the register itself; struct keys pass the bytes' address through
// the register (per the IR builder's struct-argument convention).
static inline const u32* map_key_src_from_regs(const MapHeader* header, const u64* regs, u8 first_arg) {
    if (header->key_is_inline) {
        return reinterpret_cast<const u32*>(&regs[first_arg + 1]);
    }
    return reinterpret_cast<const u32*>(regs[first_arg + 1]);
}

// Read the source pointer for an inserted value from the argument registers.
// For inline (primitive) values, the value bytes live directly in the register
// array starting at regs[first_arg + 2]. For struct values, the register holds
// a pointer to the bytes.
static inline const u32* map_value_src_from_regs(const MapHeader* header, const u64* regs, u8 first_arg) {
    if (header->value_is_inline) {
        return reinterpret_cast<const u32*>(&regs[first_arg + 2]);
    }
    return reinterpret_cast<const u32*>(regs[first_arg + 2]);
}

static void native_map_contains(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_contains: null map reference";
        return;
    }
    const MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    regs[dst] = map_contains(vm, map_ptr, key_src) ? 1 : 0;
}

// Pack the map's value bytes into the destination register(s). For inline
// (primitive) values, pack into a single u64. For struct values, return a
// pointer to the value bytes in the map's backing storage — stable until the
// next insert/remove/clear; callers that materialize a struct copy via
// STRUCT_LOAD_REGS do so immediately after the call.
static inline void map_write_value_to_regs(const MapHeader* header, const u32* src,
                                           u64* regs, u8 dst) {
    if (header->value_is_inline) {
        if (header->value_slot_count == 1) {
            // Sign-extend 1-slot (≤ 32-bit) integer values to fill the 64-bit
            // register — matches the invariant maintained by LOAD_INT and the
            // arithmetic ops (which read registers via reg_as_i64).
            regs[dst] = static_cast<u64>(static_cast<i64>(static_cast<i32>(src[0])));
        } else {
            u64 packed = 0;
            memcpy(&packed, src, sizeof(u32) * header->value_slot_count);
            regs[dst] = packed;
        }
    } else {
        regs[dst] = reinterpret_cast<u64>(src);
    }
}

static void native_map_get(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_get: null map reference";
        return;
    }
    const MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    const u32* value_ptr = map_get_ptr(vm, map_ptr, key_src, &vm->error);
    if (!value_ptr) {
        return;
    }
    map_write_value_to_regs(header, value_ptr, regs, dst);
}

static void native_map_insert(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_insert: null map reference";
        return;
    }
    MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    const u32* value_src = map_value_src_from_regs(header, regs, first_arg);
    map_insert(vm, map_ptr, key_src, value_src);
    regs[dst] = 0;
}

static void native_map_remove(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_remove: null map reference";
        return;
    }
    const MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    regs[dst] = map_remove(vm, map_ptr, key_src) ? 1 : 0;
}

static void native_map_clear(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map_clear: null map reference";
        return;
    }
    map_clear(map_ptr);
    regs[dst] = 0;
}

static void native_map_keys(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
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
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map index: null map reference";
        return;
    }
    const MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    const u32* value_ptr = map_get_ptr(vm, map_ptr, key_src, &vm->error);
    if (!value_ptr) {
        return;
    }
    map_write_value_to_regs(header, value_ptr, regs, dst);
}

// Native function: map index_mut (insert/update key-value pair)
static void native_map_index_mut(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        vm->error = "map index_mut: null map reference";
        return;
    }
    MapHeader* header = get_map_header(map_ptr);
    const u32* key_src = map_key_src_from_regs(header, regs, first_arg);
    const u32* value_src = map_value_src_from_regs(header, regs, first_arg);
    map_insert(vm, map_ptr, key_src, value_src);
    regs[dst] = 0;
}

// ===== Internal map bucket iteration (for cleanup of noncopyable elements) =====

static void native_map_mark_ref_values(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (map_ptr) roxy_map_mark_ref_values(map_ptr);
}

static void native_list_mark_ref_elements(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* list_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (list_ptr) roxy_list_mark_ref_elements(list_ptr);
}

static void native_map_iter_capacity(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    if (!map_ptr) {
        regs[dst] = 0;
        return;
    }
    const MapHeader* header = get_map_header(map_ptr);
    regs[dst] = static_cast<u64>(header->capacity);
}

static void native_map_iter_next_occupied(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    i32 idx = static_cast<i32>(regs[first_arg + 1]);
    const MapHeader* header = get_map_header(map_ptr);
    u32 cap = header->capacity;
    u32 i = static_cast<u32>(idx);
    while (i < cap && header->distances[i] == 0) {
        i++;
    }
    regs[dst] = static_cast<u64>(i);
}

static void native_map_iter_key_at(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    i32 idx = static_cast<i32>(regs[first_arg + 1]);
    const MapHeader* header = get_map_header(map_ptr);
    // Used only for cleanup of noncopyable keys (always pointer-sized,
    // 2 u32 slots). Pack the leading 2 slots into a u64.
    u64 packed = 0;
    u32 copy_slots = header->key_slot_count < 2 ? header->key_slot_count : 2;
    memcpy(&packed,
           header->keys + static_cast<size_t>(idx) * header->key_slot_count,
           sizeof(u32) * copy_slots);
    regs[dst] = packed;
}

static void native_map_iter_value_at(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* map_ptr = reinterpret_cast<void*>(regs[first_arg]);
    i32 idx = static_cast<i32>(regs[first_arg + 1]);
    const MapHeader* header = get_map_header(map_ptr);
    // Used only for cleanup of noncopyable values (which are always pointer-sized,
    // 2 u32 slots). Return the pointer as a u64 regardless of value_slot_count.
    u64 packed = 0;
    u32 copy_slots = header->value_slot_count < 2 ? header->value_slot_count : 2;
    memcpy(&packed, header->values + static_cast<size_t>(idx) * header->value_slot_count,
           sizeof(u32) * copy_slots);
    regs[dst] = packed;
}

// Native function: str_char_at(s: string, i: i32) -> i32
// Returns the ASCII code of the character at the given index.
static void native_str_char_at(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);
    if (!str) {
        vm->error = "str_char_at: null string";
        return;
    }
    i32 index = static_cast<i32>(regs[first_arg + 1]);
    u32 len = string_length(str);
    if (index < 0 || static_cast<u32>(index) >= len) {
        vm->error = "str_char_at: index out of bounds";
        return;
    }
    const char* chars = string_chars(str);
    regs[dst] = static_cast<u64>(static_cast<u8>(chars[index]));
}

// Native function: str_substr(s: string, start: i32, len: i32) -> string
// Extracts a substring starting at 'start' with the given length.
static void native_str_substr(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);
    if (!str) {
        vm->error = "str_substr: null string";
        return;
    }
    i32 start = static_cast<i32>(regs[first_arg + 1]);
    i32 sub_len = static_cast<i32>(regs[first_arg + 2]);
    u32 str_len = string_length(str);
    // Bounds check without the `start + sub_len` signed-overflow UB: require
    // start <= str_len, then sub_len <= str_len - start (computed in u32, and
    // safe to subtract because the prior clause short-circuits when start is
    // past the end).
    if (start < 0 || sub_len < 0 ||
        static_cast<u32>(start) > str_len ||
        static_cast<u32>(sub_len) > str_len - static_cast<u32>(start)) {
        vm->error = "str_substr: index out of bounds";
        return;
    }
    const char* chars = string_chars(str);
    void* result = string_alloc(vm, chars + start, static_cast<u32>(sub_len));
    if (!result) {
        vm->error = "str_substr: failed to allocate string";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: str_to_f64(s: string) -> f64
// Parses a string as a floating-point number.
static void native_str_to_f64(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* str = reinterpret_cast<void*>(regs[first_arg]);
    if (!str) {
        vm->error = "str_to_f64: null string";
        return;
    }
    const char* chars = string_chars(str);
    char* end = nullptr;
    f64 val = strtod(chars, &end);
    memcpy(&regs[dst], &val, sizeof(f64));
}

// Native function: str_from_code(code: i32) -> string
// Creates a single-character string from an ASCII code.
static void native_str_from_code(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    i32 code = static_cast<i32>(regs[first_arg]);
    char ch = static_cast<char>(code);
    void* result = string_alloc(vm, &ch, 1);
    if (!result) {
        vm->error = "str_from_code: failed to allocate string";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(result);
}

// Native function: sqrt(x: f64) -> f64
static void native_sqrt(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    f64 val;
    memcpy(&val, &regs[first_arg], sizeof(f64));
    f64 result = std::sqrt(val);
    memcpy(&regs[dst], &result, sizeof(f64));
}

// Native function: clock() -> f64
// Returns current time in seconds since an arbitrary epoch.
static void native_clock(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    auto now = std::chrono::high_resolution_clock::now();
    auto duration = now.time_since_epoch();
    f64 seconds = std::chrono::duration<f64>(duration).count();
    memcpy(&regs[dst], &seconds, sizeof(f64));
}

// Native function: read_file(path: string) -> string
// Reads the entire contents of a file as a string.
static void native_read_file(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
    u64* regs = vm->call_stack_back().registers;
    void* path_str = reinterpret_cast<void*>(regs[first_arg]);
    if (!path_str) {
        vm->error = "read_file: null path";
        return;
    }
    const char* path = string_chars(path_str);
    FILE* file = fopen(path, "rb");
    if (!file) {
        vm->error = "read_file: could not open file";
        return;
    }
    fseek(file, 0, SEEK_END);
    long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0) {
        fclose(file);
        vm->error = "read_file: could not determine file size";
        return;
    }
    char* buffer = static_cast<char*>(malloc(static_cast<size_t>(size)));
    if (!buffer) {
        fclose(file);
        vm->error = "read_file: allocation failed";
        return;
    }
    size_t bytes_read = fread(buffer, 1, static_cast<size_t>(size), file);
    fclose(file);
    void* result = string_alloc(vm, buffer, static_cast<u32>(bytes_read));
    free(buffer);
    if (!result) {
        vm->error = "read_file: failed to allocate string";
        return;
    }
    regs[dst] = reinterpret_cast<u64>(result);
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
    // index borrows the element (borrowed T -> ref T for noncopyable elements,
    // T for copyable); pop transfers ownership and stays `: T`.
    registry.bind_method(native_list_index,     "fun List<T>.index(idx: i32): borrowed T");
    registry.bind_method(native_list_index_mut, "fun List<T>.index_mut(idx: i32, val: T)");
    // Explicit deep copy — containers are move-only, so `.copy()` is how you ask
    // for an independent duplicate (lifetimes.md "Applying the model").
    registry.bind_method(native_list_copy,      "fun List<T>.copy(): List<T>");

    // Free functions
    registry.bind_native(native_print, "fun print(s: string)");

    // String functions
    registry.bind_native(native_str_concat, "fun str_concat(a: string, b: string): string");
    registry.bind_native(native_str_eq, "fun str_eq(a: string, b: string): bool");
    registry.bind_native(native_str_ne, "fun str_ne(a: string, b: string): bool");
    registry.bind_native(native_str_len, "fun str_len(s: string): i32");
    registry.bind_native(native_str_char_at, "fun str_char_at(s: string, i: i32): i32");
    registry.bind_native(native_str_substr, "fun str_substr(s: string, start: i32, len: i32): string");
    registry.bind_native(native_str_to_f64, "fun str_to_f64(s: string): f64");
    registry.bind_native(native_str_from_code, "fun str_from_code(code: i32): string");

    // Utility functions
    registry.bind_native(native_clock, "fun clock(): f64");
    registry.bind_native(native_read_file, "fun read_file(path: string): string");

    // Math functions
    registry.bind_native(native_sqrt, "fun sqrt(x: f64): f64");

    // to_string natives for primitive types ($$-mangled name override)
    registry.bind_native("bool$$to_string",   native_bool_to_string,   "fun to_string(val: bool): string");
    registry.bind_native("i32$$to_string",    native_i32_to_string,    "fun to_string(val: i32): string");
    registry.bind_native("i64$$to_string",    native_i64_to_string,    "fun to_string(val: i64): string");
    registry.bind_native("f32$$to_string",    native_f32_to_string,    "fun to_string(val: f32): string");
    registry.bind_native("f64$$to_string",    native_f64_to_string,    "fun to_string(val: f64): string");
    registry.bind_native("string$$to_string", native_string_to_string, "fun to_string(val: string): string");

    // Hash natives for primitive types
    registry.bind_native("bool$$hash",   native_bool_hash,   "fun hash(val: bool): u64");
    registry.bind_native("i8$$hash",     native_i8_hash,     "fun hash(val: i8): u64");
    registry.bind_native("i16$$hash",    native_i16_hash,    "fun hash(val: i16): u64");
    registry.bind_native("i32$$hash",    native_i32_hash,    "fun hash(val: i32): u64");
    registry.bind_native("i64$$hash",    native_i64_hash,    "fun hash(val: i64): u64");
    registry.bind_native("u8$$hash",     native_u8_hash,     "fun hash(val: u8): u64");
    registry.bind_native("u16$$hash",    native_u16_hash,    "fun hash(val: u16): u64");
    registry.bind_native("u32$$hash",    native_u32_hash,    "fun hash(val: u32): u64");
    registry.bind_native("u64$$hash",    native_u64_hash,    "fun hash(val: u64): u64");
    registry.bind_native("f32$$hash",    native_f32_hash,    "fun hash(val: f32): u64");
    registry.bind_native("f64$$hash",    native_f64_hash,    "fun hash(val: f64): u64");
    registry.bind_native("string$$hash", native_string_hash, "fun hash(val: string): u64");

    // Map<K, V> - registered as a generic native type with 2 type params
    registry.register_generic_type("Map<K, V>", "map_alloc", native_map_alloc);
    // Constructor receives: key_kind (i32, hidden), capacity (i32, optional)
    registry.bind_constructor(native_map_init, "fun Map<K, V>.new(key_kind: i32, capacity: i32)", 1);
    registry.bind_generic_destructor("Map", native_map_delete);
    registry.bind_generic_copy_constructor("Map", "map_copy", native_map_copy);
    registry.bind_method(native_map_len,       "fun Map<K, V>.len(): i32");
    registry.bind_method(native_map_contains,  "fun Map<K, V>.contains(key: K): bool");
    registry.bind_method(native_map_get,       "fun Map<K, V>.get(key: K): borrowed V");
    registry.bind_method(native_map_insert,    "fun Map<K, V>.insert(key: K, val: V)");
    registry.bind_method(native_map_remove,    "fun Map<K, V>.remove(key: K): bool");
    registry.bind_method(native_map_clear,     "fun Map<K, V>.clear()");
    registry.bind_method(native_map_keys,      "fun Map<K, V>.keys(): List<K>");
    registry.bind_method(native_map_values,    "fun Map<K, V>.values(): List<V>");
    registry.bind_method(native_map_index,     "fun Map<K, V>.index(key: K): borrowed V");
    registry.bind_method(native_map_index_mut, "fun Map<K, V>.index_mut(key: K, val: V)");
    registry.bind_method(native_map_copy,      "fun Map<K, V>.copy(): Map<K, V>");

    registry.bind_native("__list_mark_ref_elements", native_list_mark_ref_elements, "fun __list_mark_ref_elements(list: i64)");

    // Internal map bucket iteration functions (used by emit_map_cleanup for noncopyable elements)
    registry.bind_native("__map_mark_ref_values",    native_map_mark_ref_values,    "fun __map_mark_ref_values(map: i64)");
    registry.bind_native("__map_iter_capacity",       native_map_iter_capacity,       "fun __map_iter_capacity(map: i64): i32");
    registry.bind_native("__map_iter_next_occupied", native_map_iter_next_occupied, "fun __map_iter_next_occupied(map: i64, idx: i32): i32");
    registry.bind_native("__map_iter_key_at",        native_map_iter_key_at,        "fun __map_iter_key_at(map: i64, idx: i32): i64");
    registry.bind_native("__map_iter_value_at",      native_map_iter_value_at,      "fun __map_iter_value_at(map: i64, idx: i32): i64");
}

}

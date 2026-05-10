# Map<K, V> — Hash Table Implementation

## Overview

`Map<K, V>` is a built-in generic hash table using Robin Hood open addressing with backward-shift deletion. It follows the same integration pattern as `List<T>`: runtime data structure + type system + semantic analysis + native registration + C++ interop.

## Memory Layout

```
[ObjectHeader (16 bytes)] [MapHeader] [separate malloc'd distance/key/value arrays]
```

`MapHeader` is the unified `roxy_map_header` from `roxy_rt.h` — both the VM and AOT-compiled programs share one definition. The relevant fields:

```c
typedef struct {
    uint32_t length;            // Number of live entries
    uint32_t capacity;          // Number of buckets (power of 2, or 0)
    uint8_t  key_kind;          // ROXY_MAP_KEY_INTEGER/FLOAT32/FLOAT64/STRING/STRUCT
    uint8_t  key_slot_count;    // u32 slots per key (2 for primitives, N for structs)
    uint8_t  key_is_inline;     // 1 = primitive packed in slots; 0 = struct (caller passes ptr)
    uint8_t  value_slot_count;  // u32 slots per value
    uint8_t  value_is_inline;
    uint8_t  _pad[3];
    roxy_map_hash_fn hash_fn;   // C function pointer for custom Hash on Struct keys (or null)
    roxy_map_eq_fn   eq_fn;     // Same for Eq
    uint32_t hash_fn_index;     // VM-only bytecode dispatch index; UINT32_MAX = unused
    uint32_t eq_fn_index;
    uint8_t*  distances;        // Per-bucket Robin Hood distance+1 (0 = empty)
    uint32_t* keys;             // Key storage: capacity * key_slot_count u32 slots
    uint32_t* values;           // Value storage: capacity * value_slot_count u32 slots
} roxy_map_header;
```

The distance/key/value arrays are separately malloc'd (like List's element buffer), allowing reallocation without moving the header. Keys and values live in variable-sized u32-slot arrays so struct keys (and struct values) can be stored inline.

For VM-mode `Map<Struct, V>` with user-defined `impl Hash` / `impl Eq`, `hash_fn`/`eq_fn` are set to the trampolines defined in `vm/map_dispatch.cpp`. The trampolines pop the topmost `MapDispatchFrame` from a thread-local stack — pushed by the VM-side map ops in `vm/map.cpp` before calling into `roxy_map_*` — and re-enter the bytecode interpreter via `call_user_function` with the struct-arg ABI packing for the user's `K::eq(self, other)` method.

## Robin Hood Open Addressing

- **Hash:** Dispatch via `MapKeyKind` (Integer, Float32, Float64, String)
- **Insert:** Linear probe from `hash & (capacity-1)`. Robin Hood swapping when probe distance of existing entry < our distance.
- **Lookup:** Probe until key match or entry with shorter distance (early termination).
- **Remove:** Backward-shift deletion (no tombstones). Shifts subsequent entries toward their ideal position to fill the gap.
- **Grow:** At ~80% load factor (`capacity * 4/5`), allocate doubled capacity and rehash.
- **Initial capacity:** 0 by default (lazy allocation on first insert, minimum 8).

## Key Types

```cpp
enum class MapKeyKind : u8 {
    Integer,    // bool, i8..i64, u8..u64, enum
    Float32,    // Normalize -0→+0, hash bit representation
    Float64,    // Normalize -0→+0, hash bit representation
    String,     // FNV-1a hash, string_equals for equality
};
```

The `MapKeyKind` is determined at compile time from the key type and passed as a hidden constructor argument. It controls hash function and equality dispatch at runtime.

### Hash Functions

- **Integers:** SplitMix64 bit mixer
- **Floats:** Normalize -0.0 → +0.0, then hash bit representation as integer
- **Strings:** FNV-1a on string bytes (matching existing StringView hash)
- **Bools:** Direct 0/1

### Struct Keys (Deferred)

Struct keys require calling Roxy `hash()` / `eq()` methods from native C code (VM re-entry). This will be implemented as a follow-up. Currently, using a struct as a Map key produces a compile-time error.

## Hash Trait

A builtin `Hash` trait is declared in semantic pass 1.7b with a required `hash(): i64` method. All primitives (bool, integers, floats, string) automatically implement it. Enums inherit Hash from their i32 underlying type.

## API

### Roxy Language

```roxy
// Construction
var m: Map<i32, string> = Map<i32, string>();       // empty
var m: Map<i32, string> = Map<i32, string>(64);     // pre-allocated capacity

// Methods
m.len()                 // -> i32: number of entries
m.contains(key)         // -> bool: key exists?
m.get(key)              // -> V: value (runtime error if missing)
m.insert(key, val)      // -> void: insert or update
m.remove(key)           // -> bool: was key present?
m.clear()               // -> void: remove all entries
m.keys()                // -> List<K>: all keys
m.values()              // -> List<V>: all values

// Index operators
var v: V = m[key];      // read (runtime error if missing)
m[key] = val;           // write (insert or update)
```

### C++ Interop

```cpp
#include "roxy/vm/binding/roxy_map.hpp"

// In a bound function receiving RoxyVM*:
auto map = RoxyMap<i32, i32>::alloc(vm, MapKeyKind::Integer, 16);
map.insert(42, 100);
bool found = map.contains(42);
i32 val = map.get(42);
map.remove(42);
```

## Copy and Move Semantics

A `Map<K, V>` is **noncopyable** when either `K` or `V` is noncopyable. Noncopyable maps follow the same move-semantic rules as noncopyable lists and `uniq` variables:

- **Passing to a function** moves ownership
- **Initializing a new variable** moves the source
- **Use-after-move** is a compile-time error
- **Struct fields** of noncopyable map type trigger a synthetic destructor on the containing struct

When both `K` and `V` are copyable, the map is freely copyable via `map_copy` in the function prologue.

### Scope-Exit Cleanup (RAII)

When a noncopyable map goes out of scope, the compiler emits cleanup IR:

1. If `K` is noncopyable: call `Map$$keys` to extract keys into a temp list, run a cleanup loop on each key, free the temp list
2. If `V` is noncopyable: call `Map$$values` similarly, clean up each value
3. Call `Map$$delete` to free the map's internal buffers
4. Call `Delete` to free the slab-allocated map header

## Implementation Files

| File | Description |
|------|-------------|
| `include/roxy/rt/roxy_rt.h` | Unified `roxy_map_header`, `roxy_map_*` C API |
| `src/roxy/rt/roxy_rt.cpp` | Robin Hood hash table impl shared between VM and AOT |
| `include/roxy/vm/map.hpp` | `MapHeader` typedef alias of `roxy_map_header`, MapKeyKind, VM-side declarations |
| `src/roxy/vm/map.cpp` | Thin shims around `roxy_map_*` that push/pop dispatch frames |
| `include/roxy/vm/map_dispatch.hpp` | `MapDispatchFrame`, trampoline getters |
| `src/roxy/vm/map_dispatch.cpp` | Thread-local dispatch stack + `vm_hash_trampoline`/`vm_eq_trampoline` |
| `include/roxy/vm/binding/roxy_map.hpp` | `RoxyMap<K,V>` typedef alias of `roxy::Map<K,V>` |
| `include/roxy/vm/binding/registry.hpp` | NativeParamWrapper for container returns |
| `src/roxy/vm/binding/registry.cpp` | resolve_param_desc wrapper support |
| `include/roxy/compiler/types.hpp` | TypeKind::Map, MapTypeInfo |
| `src/roxy/compiler/types.cpp` | map_type() interning, lookup_map_method |
| `include/roxy/compiler/type_env.hpp` | m_hash_type storage |
| `src/roxy/compiler/semantic.cpp` | Hash trait, Map type resolution, methods |
| `src/roxy/vm/natives.cpp` | Hash + Map native function implementations |
| `src/roxy/vm/natives.cpp` | Map index/index_mut native functions |
| `src/roxy/compiler/ir_builder.cpp` | Map constructor + method call generation |
| `tests/e2e/test_maps.cpp` | E2E tests |

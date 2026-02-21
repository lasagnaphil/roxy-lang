# Map<K, V> — Hash Table Implementation

## Overview

`Map<K, V>` is a built-in generic hash table using Robin Hood open addressing with backward-shift deletion. It follows the same integration pattern as `List<T>`: runtime data structure + type system + semantic analysis + native registration + C++ interop.

## Memory Layout

```
[ObjectHeader (16 bytes)] [MapHeader] [separate malloc'd distance/key/value arrays]
```

```cpp
struct MapHeader {
    u32 length;          // Number of live entries
    u32 capacity;        // Number of buckets (always power of 2, 0 if not allocated)
    MapKeyKind key_kind; // Dispatch tag for hash/equality
    u8* distances;       // Per-bucket Robin Hood distance+1 (0 = empty)
    u64* keys;           // Key storage (capacity entries, stored as u64)
    u64* values;         // Value storage (capacity entries, stored as u64)
};
```

The distance/key/value arrays are separately malloc'd (like List's element buffer), allowing reallocation without moving the header. Keys and values are stored as `u64` (same as register file representation).

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

## Implementation Files

| File | Description |
|------|-------------|
| `include/roxy/vm/map.hpp` | MapHeader, MapKeyKind, C API declarations |
| `src/roxy/vm/map.cpp` | Robin Hood hash table implementation |
| `include/roxy/vm/binding/roxy_map.hpp` | RoxyMap<K,V> C++ wrapper |
| `include/roxy/vm/binding/registry.hpp` | NativeParamWrapper for container returns |
| `src/roxy/vm/binding/registry.cpp` | resolve_param_desc wrapper support |
| `include/roxy/compiler/types.hpp` | TypeKind::Map, MapTypeInfo |
| `src/roxy/compiler/types.cpp` | map_type() interning, lookup_map_method |
| `include/roxy/compiler/type_env.hpp` | m_hash_type storage |
| `src/roxy/compiler/semantic.cpp` | Hash trait, Map type resolution, methods |
| `src/roxy/vm/natives.cpp` | Hash + Map native function implementations |
| `src/roxy/vm/interpreter.cpp` | GET_INDEX/SET_INDEX type dispatch |
| `src/roxy/compiler/ir_builder.cpp` | Map constructor + method call generation |
| `tests/e2e/maps_test.cpp` | E2E tests |

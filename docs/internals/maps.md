# Map<K, V> — Hash Table Implementation

`Map<K, V>` is a built-in generic hash table using Robin Hood open addressing with backward-shift deletion. It follows the same integration pattern as `List<T>`: a runtime data structure plus type-system, semantic-analysis, native-registration, and C++ interop support.

## Memory Layout

```
[ObjectHeader (16 bytes)] [MapHeader] [separate malloc'd distance/key/value arrays]
```

`MapHeader` is the unified `roxy_map_header` from `rt/roxy_rt.h`, shared by the VM and AOT-compiled programs. It holds `length`/`capacity`, per-kind key/value metadata (`key_kind`, slot counts, inline flags), optional `hash_fn`/`eq_fn` C function pointers for struct keys, and pointers to three separately malloc'd arrays: per-bucket Robin Hood `distances`, `keys`, and `values`. Keys and values use variable-sized u32-slot arrays so struct keys and struct values can be stored inline, and the side arrays reallocate on growth without moving the header. See `rt/roxy_rt.h` for the exact fields.

## Robin Hood Open Addressing

- **Hash:** dispatch via `MapKeyKind` (Integer, Float32, Float64, String, Struct).
- **Insert:** linear probe from `hash & (capacity-1)`, with Robin Hood swapping when an existing entry's probe distance is shorter than ours.
- **Lookup:** probe until key match or an entry with a shorter distance (early termination).
- **Remove:** backward-shift deletion (no tombstones) — shifts subsequent entries toward their ideal position to fill the gap.
- **Grow:** at ~80% load factor (`capacity * 4/5`), allocate doubled capacity and rehash.
- **Initial capacity:** 0 by default (lazy allocation on first insert, minimum 8).

## Key Types

```cpp
enum class MapKeyKind : u8 {
    Integer,    // bool, i8..i64, u8..u64, enum
    Float32,    // normalize -0→+0, hash bit representation
    Float64,    // normalize -0→+0, hash bit representation
    String,     // hash via cached header field, string_equals for equality
    Struct,     // bytewise FNV-1a + memcmp, or user-defined Hash/Eq trait methods
};
```

`MapKeyKind` is determined at compile time from the key type and passed as a hidden constructor argument; it controls hash and equality dispatch at runtime. Hash functions per kind: integers use a SplitMix64 bit mixer; floats normalize `-0.0 → +0.0` then hash the bit representation; strings read the XXH3 hash cached in the string header (see `strings.md`) — no re-hash per probe; bools use 0/1 directly.

### Struct Keys

Struct keys are supported. A struct used as a `Map` key hashes and compares via its `Hash` / `Eq` trait methods, or — with no user-defined impl — a bytewise FNV-1a hash plus `memcmp`. In VM mode, `hash_fn`/`eq_fn` point at the trampolines in `vm/map_dispatch.cpp`: each trampoline pops the topmost `MapDispatchFrame` from a thread-local stack (pushed by the VM-side map ops in `vm/map.cpp` before calling into `roxy_map_*`) and re-enters the bytecode interpreter via `call_user_function`, packing struct args for the user's `K::hash(self)` / `K::eq(self, other)` method.

The frame's bytecode-function indices (`hash_fn_idx` / `eq_fn_idx`) live in a per-VM side-table — `tsl::robin_map<void*, MapDispatchInfo> map_dispatch` on `RoxyVM`, keyed by map pointer. Entries are inserted at `map_alloc` and removed by the map's destructor when the slab reclaims the header, so a recycled slab slot can't inherit stale dispatch indices.

## Hash Trait

A builtin `Hash` trait is declared in semantic pass 1.7b with a required `hash(): i64` method. All primitives (bool, integers, floats, string) automatically implement it; enums inherit Hash from their i32 underlying type.

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

`index` is typed `fun Map<K, V>.index(key: K): borrowed V`: the `borrowed` modifier yields a borrow of the value rather than transferring it. For a noncopyable `V` (e.g. `Map<i32, uniq Point>`) the result is `ref Point`, so `var x: uniq Point = m[k]` is a `ref → uniq` type error; for copyable `V` it is just `V` (a copy). See [lifetimes.md §17](lifetimes.md#the-borrowed-type-modifier).

### C++ Interop

Include `roxy/vm/binding/roxy_map.hpp`. `RoxyMap<K, V>` is an alias of `roxy::Map<K, V>`; bound C++ functions take no `RoxyVM*` (the runtime context is thread-local). Allocate with `RoxyMap<K,V>::alloc((i32)MapKeyKind::Integer, capacity)`, then use `insert` / `contains` / `get` / `remove`.

## Copy and Move Semantics

A `Map<K, V>` is **noncopyable** when either `K` or `V` is noncopyable. Noncopyable maps follow the same move-semantic rules as noncopyable lists and `uniq` variables:

- **Passing to a function** moves ownership.
- **Initializing a new variable** moves the source.
- **Use-after-move** is a compile-time error.
- **Struct fields** of noncopyable map type trigger a synthetic destructor on the containing struct.

When both `K` and `V` are copyable, the map is freely copyable via `map_copy` in the function prologue.

### Scope-Exit Cleanup (RAII)

When a noncopyable map goes out of scope, the compiler emits cleanup IR:

1. If `K` is noncopyable: call `Map$$keys` to extract keys into a temp list, run a cleanup loop on each key, free the temp list.
2. If `V` is noncopyable: call `Map$$values` similarly, clean up each value.
3. Call `Map$$delete` to free the map's internal buffers.
4. Call `Delete` to free the slab-allocated map header.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/rt/roxy_rt.h` | Unified `roxy_map_header`, `roxy_map_*` C API |
| `src/roxy/rt/roxy_rt.cpp` | Robin Hood hash table impl shared between VM and AOT |
| `src/roxy/vm/map.cpp` | Thin shims around `roxy_map_*` that push/pop dispatch frames |
| `src/roxy/vm/map_dispatch.cpp` | Thread-local dispatch stack + `vm_hash_trampoline` / `vm_eq_trampoline` |
| `src/roxy/compiler/semantic.cpp` | Hash trait, Map type resolution, methods |
| `src/roxy/compiler/ir_builder.cpp` | Map constructor + method call generation |
| `src/roxy/vm/natives.cpp` | Hash + Map native function implementations |
| `tests/e2e/test_maps.cpp` | E2E tests |

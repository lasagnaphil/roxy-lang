# Map<K, V> — Hash Table Implementation

`Map<K, V>` is a built-in generic hash table using Robin Hood open addressing with backward-shift deletion. It is a generic native type registered like `List<T>` ([list.md](list.md)); the method set and signatures are the `bind_method` calls in `register_builtin_natives` (`src/roxy/vm/natives.cpp`), and the implementation is shared by the VM and AOT in `rt/roxy_rt.{h,cpp}`.

## Memory Layout

A slab object `[ObjectHeader][roxy_map_header]` pointing at three separately malloc'd per-bucket arrays: Robin Hood `distances`, `keys`, and `values`. Keys and values are variable-width u32-slot arrays so struct keys/values live **inline** in the bucket, and the arrays reallocate on growth without moving the header (same reason as `List`).

## Robin Hood Open Addressing

Power-of-two capacity, lookup terminates early at an entry with a shorter probe distance, **backward-shift deletion (no tombstones)**, grow ×2 at 80% load, lazy allocation on first insert (minimum 8).

## Key Kinds

`MapKeyKind` (`vm/map.hpp`) is determined at compile time from the key type and passed as a **hidden constructor argument**; it drives hash/equality dispatch at runtime. Integers (incl. bool, enum) use a SplitMix64 mixer; floats normalize `-0.0 → +0.0` and hash the bits; strings reuse the XXH3 hash cached in the string header ([strings.md](strings.md)), so probes never re-hash.

### Struct Keys

A struct key hashes and compares via its `Hash` / `Eq` trait methods, or — with no user impl — bytewise FNV-1a + `memcmp`. The runtime only sees C function pointers (`hash_fn`/`eq_fn` in the header). In VM mode those are trampolines (`vm/map_dispatch.cpp`) that re-enter the interpreter: the VM-side map ops (`vm/map.cpp`) push a `MapDispatchFrame` on a thread-local stack before calling `roxy_map_*`, and the trampoline reads it to find the user's bytecode `hash`/`eq`. The per-map function indices live in `RoxyVM::map_dispatch`, keyed by map pointer, and are erased by the map's destructor so a recycled slab slot can't inherit stale indices.

The builtin `Hash` trait (required `hash(): u64`) is implemented by all primitives; enums hash as their i32 discriminant.

## Counted keys

A `string` key is reference-counted and the map is one of its owners, so
`roxy_map_insert` acquires a count, `roxy_map_remove` / `roxy_map_clear` release
it, `roxy_map_keys` acquires for the `List<K>` it produces, and the teardown
descriptor releases whatever is left — otherwise a key built per iteration
(`m.insert(f"k{i}", v)`) would die with the loop body.

Key counting lives in the **runtime**, unlike map *values*, which are counted by
emitted IR. Only the runtime can see which key is actually stored: `insert`
replaces in place and keeps the key it already holds, so the incoming key must
not be retained on that path, and `remove` has to release the *stored* key rather
than the caller's equal-valued copy. `key_kind` gives the runtime everything it
needs, exactly as `value_is_ref` does for borrowed values.

`values()` and `copy()` produce a container that shares the original's elements,
so the compiler emits a retain loop over the result — the counterpart to
`keys()`, which the runtime handles because keys are a closed set of kinds.

A *copyable struct* key holding a counted member is deliberately unsupported: the
runtime cannot walk it to acquire, and such a key could never match on lookup
anyway, because `map_keys_equal` compares key bytes. The drop descriptor's key
gate matches, dropping only a move-only key (moved in, so nothing was acquired)
or a `string`.

## Reads and missing keys

Reading a missing key with `m[key]` **throws a catchable `KeyError`** (see
[exceptions.md → Index-operator exceptions](exceptions.md#index-operator-exceptions));
`m.get(key)` still aborts, so use `m[key]` inside a `try`/`catch` — or `get_or` —
when a miss is possible. The read lowers to a single Robin-Hood probe: the IR
builder emits `IndexTryAddr` (VM `INDEX_TRYADDR_MAP` / C `roxy_map_get_or` with a
null fallback), branches on a null value-slot pointer to a `throw KeyError`
block, and otherwise loads the value from the pointer — no second lookup. (An
`inout m[key]` borrow of a missing key still traps, not throws — the lvalue path
uses `INDEX_ADDR_MAP`.)

`index` (and `get`) return `borrowed V`: a borrow for noncopyable `V`, a copy otherwise — see [lifetimes.md → The `borrowed` type modifier](lifetimes.md#the-borrowed-type-modifier).

`get_or(key, fallback)` is the missing-key-tolerant read: one probe, returns the stored value or `fallback`, never aborts and never inserts. It is **restricted to copyable `V`** — a move-only value can't be copied out, and returning the stored one would alias the map's owned storage. `roxy_map_get_or` returns a pointer to either the found value or the passed-in fallback bytes; on the VM `native_map_get_or` stages inline values through a local buffer because the result register may overlap the argument window holding the fallback.

## Printable: the synthesized `to_string`

`Map<K, V>` implements `Printable` structurally (iff both `K` and `V` do).
`f"{m}"`, `m.to_string()`, and `print(m)` format as `{k1: v1, k2: v2}` (`{}`
when empty) in **Robin-Hood bucket order — unspecified** by design. The
conversion is a compiler-synthesized per-instantiation IR function
(`Map$string$i32$$to_string`; see the twin section in [list.md](list.md)) iterating
occupied buckets via the `__map_iter_*` natives. Because struct keys/values live
inline in the bucket, every compiler-emitted walk (this one and the `values()`/`copy()`
retain loop) must use the `*_ptr_at` variants (interior pointers), never the packed
`*_at` accessors, which are for pointer-shaped/scalar values only; on the C backend
packed f32/f64 results are bit-reinterpreted via `memcpy`, not value-cast.

C++ interop: `roxy/vm/binding/roxy_map.hpp` (see [interop.md](interop.md)).

## Copy and Move Semantics

A `Map<K, V>` is **always noncopyable**, whatever `K` and `V` are: it owns a heap bucket array, so — like `List` and `uniq` — it is move-only ([lifetimes.md](lifetimes.md)). `.copy()` produces an independent duplicate when one is wanted (rejected when `K` or `V` isn't copyable).

A `ref Map<K, V>` parameter **borrows** exactly as `ref List<T>` does ([list.md](list.md)). See [lifetimes.md → Containers are borrowable](lifetimes.md#containers-are-borrowable).

### Scope-Exit Cleanup (RAII)

A single typed `Delete` (`DropKind::Map`) drops each occupied bucket's key and value where needed, then frees the arrays and header. See
[lifetimes.md → One derivation, two executions](lifetimes.md#one-derivation-two-executions).

## Files

Runtime: `rt/roxy_rt.{h,cpp}`. VM shims that push dispatch frames: `vm/map.cpp`; trampolines: `vm/map_dispatch.cpp`. Tests: `tests/e2e/test_maps.cpp`.

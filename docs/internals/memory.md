# Memory Model

Roxy uses a reference-counted memory model with three reference types and no garbage collector. Heap objects are managed by a custom slab allocator with Vale-style random generational references for weak-reference soundness.

## Reference Types

| Type | Owns? | Nullable? | On dangling |
|------|-------|-----------|-------------|
| `uniq` | Yes | Yes | N/A (is owner) |
| `ref` | No | No | Assert/crash |
| `weak` | No | Yes | Returns null or asserts |

### Critical Rule: No `ref` in Fields

To prevent reference cycles, `ref` is restricted to function parameters and local variables. Struct fields must use `uniq` (ownership) or `weak` (back-references).

## Object Header

Every heap-allocated object is prefixed by a 16-byte `ObjectHeader`: a 64-bit random `weak_generation` (0 = dead/tombstoned), a `ref_count` of active `ref` pointers, and a `type_id` for runtime type info. `is_alive()` is `weak_generation != 0`. The unified definition lives in `roxy_rt.h` as `roxy_object_header`.

## Slab Allocator

Heap objects are allocated from fixed-size slabs chosen by object size:

| Class | Slot Size |
|-------|-----------|
| 0 | 32 bytes |
| 1 | 64 bytes |
| 2 | 128 bytes |
| 3 | 256 bytes |
| 4 | 512 bytes |
| 5 | 1024 bytes |
| 6 | 2048 bytes |
| 7 | 4096 bytes |
| 8+ | Large objects (multiple pages) |

The allocator backs slabs with platform-specific virtual memory operations (`reserve`/`commit`/`decommit`/`release`/`remap_to_zero`/`page_size`); see `include/roxy/rt/vmem.hpp`.

### Tombstoning and Recycling

When an object is freed:
1. The entire slot (header + data) is zeroed, so `weak_generation` reads as 0 and `is_alive()` returns false.
2. The slot is pushed back onto its slab's intrusive free list, ready for the next allocation in that size class.
3. Memory stays mapped throughout, so weak references can keep dereferencing safely (they see `is_alive() == false` until the slot is re-allocated).

The intrusive next-pointer sits at offset `sizeof(ObjectHeader)` (past the header), not offset 0, so `weak_generation` keeps reading as zero while the slot is parked on the free list.

Stale-weak-reference safety after recycle comes from the 64-bit random `weak_generation`: a reused slot gets a fresh random generation, so any weak reference still holding the old generation mismatches and returns false. Collision probability is 2⁻⁶⁴ per recycle.

### Slab Reclamation

Recycling solves slot-level fragmentation, but a slab whose live set has shrunk to zero still occupies physical memory. `reclaim_tombstoned()` scans all slabs and reclaims drained ones (`live_count == 0`), returning the number of pages reclaimed. For each reclaimable slab it calls `remap_to_zero()` on the whole slab (releases physical memory, keeps the vaddr mapped as zeros), sets `free_head = 0xFFFFFFFF` so no further slots are handed out, and marks the slab `remapped`. The `remapped` flag makes repeated reclamation passes idempotent.

## Random Generational References

Weak references validate against 64-bit random generations (Vale-style) rather than incrementing counters: random generations prevent reuse attacks and avoid 32-bit wrap-around. The PRNG is xorshift128+ (`RandomGen`, seeded via SplitMix64 from a high-resolution timer plus process ID).

`weak_ref_valid(data, generation)` returns false on a null pointer; otherwise it reads the header (always safe — memory stays mapped whether active or tombstoned) and returns `is_alive() && weak_generation == generation`. Ref counting, weak-ref create/validate, and object alloc/free are declared in `include/roxy/vm/object.hpp`.

## Constraint Reference Model

Roxy uses a "constraint reference" model:

1. `uniq` owns the object but doesn't affect `ref_count`.
2. Creating a `ref` borrow increments `ref_count`; destroying it decrements.
3. `delete` fails at runtime if `ref_count > 0`.

This prevents use-after-free while allowing flexible borrowing.

## Implicit Destruction (RAII)

`uniq` variables are automatically cleaned up at scope exit, eliminating manual `delete` in most cases. The same applies to value-type structs with destructors and to noncopyable containers (`List<T>` / `Map<K,V>` where `T`, `K`, or `V` is noncopyable).

### Scope Exit Cleanup

At every scope exit point the compiler emits implicit cleanup for all live noncopyable locals declared in that scope, in LIFO (reverse declaration) order:

| Exit Point | What's Cleaned Up |
|------------|-------------------|
| End of block `{ ... }` | Uniqs declared in that block |
| `return` | All uniqs in all enclosing scopes |
| `break` | Uniqs in scopes inside the loop |
| `continue` | Uniqs in scopes inside the loop body |
| End of function (implicit return) | All uniqs in function scope |

If the struct type has a destructor (`fun delete StructName()`), it runs before the memory is freed. For noncopyable containers, the compiler emits a cleanup loop that destroys each element before freeing the container's buffers and slab header (see `docs/internals/list.md` and `docs/internals/maps.md`).

### Null Safety

`DEL_OBJ` on a null pointer is a safe no-op, so `var x: uniq T = nil;` cleans up safely and moved variables (null-ified after move) never double-free.

```roxy
fun process(): i32 {
    var p: uniq Point = uniq Point();
    p.x = 42;
    return p.x;
    // p is implicitly deleted here (before the function actually returns)
}
```

## Move Semantics

Passing a noncopyable variable to a function parameter of matching noncopyable type **moves** ownership to the callee; the caller's variable becomes invalid. This applies to `uniq` references, value-type structs with destructors, and noncopyable containers (`List<T>` / `Map<K,V>` with noncopyable inner types).

### Rules

- Passing a noncopyable value to a matching parameter → ownership transferred, caller's variable consumed.
- Returning a noncopyable value → ownership transferred to caller, variable not deleted at scope exit.
- Initializing a new variable from a noncopyable source (`var copy = items`) → ownership transferred.
- Explicit `delete` → variable consumed.
- Reassigning a noncopyable variable → old value is implicitly destroyed before the new value is assigned.

### Use-After-Move Detection

The semantic analyzer tracks a move state per noncopyable local; using a `Moved` or `MaybeValid` variable is a compile-time error:

| State | Meaning |
|-------|---------|
| `Live` | Variable owns a valid value |
| `Moved` | Ownership transferred — use is a compile error |
| `MaybeValid` | Conditionally moved (e.g., moved in one `if` branch but not the other) |

```roxy
fun main(): i32 {
    var p: uniq Point = uniq Point();
    p.x = 42;
    var result: i32 = consume(p);  // p is moved
    // p.x here would be a compile error: "use of moved value 'p'"
    return result;
}
```

Reassignment auto-deletes the old value: after `p = uniq Point()`, the previously-owned object is destroyed and `p` owns the new one, which is itself deleted at scope exit.

## The `borrowed` Type Modifier

`borrowed T` is a **resolve-time type transform** that demotes an owning type to a borrow. It lets a function or method express "I return a *view* of this, not ownership" — most importantly the container subscript operator, where returning an owning `uniq T` by alias would double-free (the caller and the container both free it).

The mapping (`TypeCache::borrowed`):

| `borrowed X` | → | rationale |
|---|---|---|
| `uniq T` | `ref T` | borrow the heap pointee instead of transferring it |
| `fun(...) -> R` | `ref fun(...) -> R` | a closure value is a heap env pointer; `ref fun` shares its representation and is callable |
| copyable `T` | `T` | a copy aliases nothing; no demotion needed |
| `ref T` / `weak T` | unchanged | already a borrow |
| other noncopyable (value struct, coroutine, `List`/`Map`) | unchanged *(prototype)* | see below |

`borrowed` is a **soft keyword** recognized only in type position (it stays usable as an ordinary identifier elsewhere); it never persists as a `Type` — resolution maps it to a concrete type, and it rides on `TypeExpr` through generic substitution so `borrowed T` resolves per monomorphization. The native `List`/`Map` `index` (and `Map.get`) methods are typed `borrowed T` / `borrowed V`, so `var x: uniq Point = list[i]` is a plain `ref → uniq` type error.

**Callable borrows.** A `ref fun` / `weak fun` borrows a closure value, which is a pointer to a heap-allocated env (with a header). Because it shares the env-pointer representation of `fun`, it is callable: the call paths (`analyze_regular_fun_call`, the IR builder's indirect-call dispatch) unwrap the borrow via `base_type()` before reading the call index. So `List<fun>` indexing yields a `ref fun` that can be both stored and invoked, without moving the closure out from under the list.

**Prototype scope.** `borrowed` demotes `uniq` and `fun` (and copies copyables); for the remaining noncopyable kinds (inline value structs, coroutines, `List`/`Map`) it is the identity, and the move-checker's native-index guard (`consume_noncopyable`) remains the backstop that rejects unsound *binds* of those while still allowing in-place use (field reads, method calls). A fuller `borrowed` would also demote those — value structs to a compile error (no object header to borrow against; store behind `uniq`), coroutines/containers to `ref` once their `ref`-receiver dispatch lands. (Passing a bare `fun` to a `ref fun` parameter still needs a `fun → ref fun` conversion, which is not yet implemented — borrows arrive via `borrowed`-typed returns for now.)

## Files

| File | Purpose |
|------|---------|
| `include/roxy/vm/object.hpp` | `ObjectHeader` (alias of `roxy_object_header`), ref-counting declarations |
| `src/roxy/vm/object.cpp` | Object allocation and ref counting |
| `include/roxy/rt/roxy_rt.h` | Unified `roxy_object_header` + `roxy_allocator` vtable type |
| `src/roxy/rt/roxy_rt.cpp` | `roxy_alloc`/`roxy_free` dispatching through ctx; default malloc allocator |
| `include/roxy/rt/vmem.hpp` | Virtual memory operations interface |
| `src/roxy/rt/vmem_{win32,unix}.cpp` | Platform virtual memory implementations |
| `include/roxy/rt/slab_allocator.hpp` | Slab allocator declarations + vtable adapter |
| `src/roxy/rt/slab_allocator.cpp` | Slab allocator implementation; `make_slab_allocator_vtable` |

The slab allocator lives in `roxy_rt` (previously under `vm/`). VM mode plugs a per-VM `SlabAllocator` into `roxy_ctx.allocator` via the vtable; AOT mode does the same with a process-wide slab created by `roxy_rt_init`. Both paths get identical generation-based weak-ref soundness — the malloc fallback only kicks in when `roxy_rt_init` hasn't been called and no ctx is active.

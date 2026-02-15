# Memory Model

Roxy uses a reference-counted memory model with three reference types and no garbage collector.

## Reference Types

| Type | Owns? | Nullable? | On dangling |
|------|-------|-----------|-------------|
| `uniq` | Yes | No | N/A (is owner) |
| `ref` | No | No | Assert/crash |
| `weak` | No | Yes | Returns null or asserts |

### Critical Rule: No `ref` in Fields

To prevent reference cycles, `ref` can only be used for:
- Function parameters
- Local variables

Struct fields must use `uniq` (ownership) or `weak` (back-references).

## Object Header

Every heap-allocated object has a header (16 bytes):

```cpp
struct ObjectHeader {
    u64 weak_generation;    // 64-bit random generation; 0 = dead/tombstoned
    u32 ref_count;          // Number of active 'ref' pointers
    u32 type_id;            // Type identifier for runtime type info

    bool is_alive() const { return weak_generation != 0; }
    void* data();           // Get pointer to object data (after header)
};
```

## Slab Allocator

Roxy uses a custom slab allocator for heap objects with the following features:

### Size Classes

Objects are allocated from fixed-size slabs based on their size:

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

### Virtual Memory

The allocator uses platform-specific virtual memory operations:

```cpp
struct VirtualMemoryOps {
    static void* reserve(u64 size);           // Reserve address space
    static bool commit(void* addr, u64 size); // Commit physical memory
    static bool decommit(void* addr, u64 size); // Release physical memory
    static void release(void* addr, u64 size);  // Release address space
    static bool remap_to_zero(void* addr, u64 size); // Zero + read-only
    static u64 page_size();                   // System page size
};
```

### Tombstoning

When an object is freed, it becomes a "tombstone":
1. `weak_generation` is set to 0 (marking the object as dead)
2. The entire slot (header + data) is zeroed by the slab allocator
3. Memory remains mapped (read-only zeros on some platforms)
4. Weak references can safely check the tombstoned memory (`is_alive()` returns false)

This allows weak references to safely detect invalidation without crashes.

### Slab Reclamation

Over time, tombstoned slots accumulate and consume physical memory. The allocator provides a reclamation mechanism to release physical memory from fully tombstoned slabs:

```cpp
// Scan all slabs and reclaim fully tombstoned ones
// Returns number of pages reclaimed
u32 reclaim_tombstoned();
```

A slab is reclaimable when:
- All slots have been allocated at some point (no free slots remaining)
- All allocated objects have been freed (all slots are tombstoned)

Reclamation calls `remap_to_zero()` on the slab, which:
- Releases physical memory back to the OS
- Keeps virtual addresses mapped for safe weak reference reads
- Marks the slab as `remapped` to avoid redundant reclamation

The `remapped` flag ensures idempotency - calling `reclaim_tombstoned()` multiple times is safe and efficient.

## Random Generational References

Weak references use 64-bit random generations (Vale-style) for validation:

### Why Random Generations?

- **64-bit random** prevents generation reuse attacks
- **No wrap-around** issues (unlike 32-bit incrementing counters)
- **xorshift128+** PRNG is fast and has good statistical properties

### Random Number Generator

```cpp
struct RandomGen {
    u64 state[2];

    void seed(u64 s);   // SplitMix64 initialization
    u64 next();         // xorshift128+ algorithm
};
```

Seeding uses high-resolution timer + process ID for uniqueness.

## Reference Counting Operations

```cpp
// Reference counting operations
void ref_inc(void* data);
bool ref_dec(RoxyVM* vm, void* data);

// Weak reference operations (64-bit generation)
u64 weak_ref_create(void* data);
bool weak_ref_valid(void* data, u64 generation);

// Object allocation/deallocation
void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size);
void object_free(RoxyVM* vm, void* data);
```

### Weak Reference Validation

```cpp
bool weak_ref_valid(void* data, u64 generation) {
    if (data == nullptr) return false;
    // Safe to read: memory is always mapped (active or tombstoned)
    ObjectHeader* header = get_header_from_data(data);
    return header->is_alive() && (header->weak_generation == generation);
}
```

## Constraint Reference Model

Roxy uses a "constraint reference" model where:

1. `uniq` owns the object but doesn't affect `ref_count`
2. `ref` borrows increment `ref_count` at creation, decrement at destruction
3. `delete` fails at runtime if `ref_count > 0`
4. This prevents use-after-free while allowing flexible borrowing

## Files

- `include/roxy/vm/object.hpp` - Object header and ref counting declarations
- `src/roxy/vm/object.cpp` - Object allocation and ref counting implementation
- `include/roxy/vm/vmem.hpp` - Virtual memory operations interface
- `src/roxy/vm/vmem_win32.cpp` - Windows virtual memory implementation
- `src/roxy/vm/vmem_unix.cpp` - Unix virtual memory implementation
- `include/roxy/vm/slab_allocator.hpp` - Slab allocator declarations
- `src/roxy/vm/slab_allocator.cpp` - Slab allocator implementation

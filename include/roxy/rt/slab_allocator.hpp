#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/rt/roxy_rt.h"

namespace rx {

// State of a slot within a slab. Freed slots go straight back to FREE
// and are recycled by future allocations. Weak references stay safe
// across recycle because the 64-bit random weak_generation differs
// (with overwhelming probability) between the old and new occupants;
// during the gap between free and re-alloc the slot's weak_generation
// reads as zero, so is_alive() returns false. There is no TOMBSTONE
// bookkeeping state — see free_in_slab() for the freeing path.
enum class SlotState : u8 {
    FREE,       // Available for allocation
    ALIVE,      // Currently in use
};

// Random number generator (xorshift128+)
// Used to generate 64-bit random generations for weak references
struct RandomGen {
    u64 state[2];

    // Initialize the generator with a seed
    void seed(u64 s);

    // Generate the next random 64-bit value
    u64 next();

private:
    // SplitMix64 for seeding
    static u64 splitmix64(u64 x);
};

// A slab is a contiguous region of memory divided into fixed-size slots
struct Slab {
    void* base_addr;            // Start of slab memory
    u32 page_count;             // Number of pages in this slab
    u32 slot_size;              // Size of each slot (includes ObjectHeader)
    u32 slot_count;             // Total slots in slab
    u32 free_head;              // Index of first free slot (free list, 0xFFFFFFFF = empty)
    u32 live_count;             // Number of ALIVE objects
    bool remapped;              // True if slab has been remapped to zeros (reclaimed)
    Vector<SlotState> states;   // Per-slot state

    // Get pointer to a specific slot
    void* slot_ptr(u32 index) const {
        return reinterpret_cast<u8*>(base_addr) + static_cast<u64>(index) * slot_size;
    }

    // True when no live objects remain. Such a slab is eligible for
    // physical-memory reclamation (remap_to_zero); future allocations
    // will skip it once reclaim_tombstoned() unlinks it from the free
    // list. This used to also require free_head == 0xFFFFFFFF (every
    // slot transitioned through ALIVE at least once, none on free list)
    // back when freed slots were never recycled — that condition was
    // rarely met under mixed-lifetime workloads, leading to permanent
    // fragmentation. With recycling, live_count == 0 alone is enough.
    bool is_drained() const {
        return live_count == 0;
    }
};

// Tracking info for a large object (> 4KB)
struct LargeObjectInfo {
    u32 page_count : 31;   // max ~2B pages (~8 TB @ 4KB pages)
    u32 tombstoned : 1;    // true after free_large; vaddr still mapped (zeros)
};

// Entry in the sorted slab-range index used by find_slab_containing().
// Slab address ranges are non-overlapping (each is a separate vmem
// reservation), so a vector sorted by `base` allows binary search:
// upper_bound on the input pointer yields the first range whose base is
// strictly greater; the previous entry is the only candidate, and its
// `end` half-open bound either confirms or rejects containment in O(1).
//
// Entries are appended-then-sorted on slab creation; slabs are never
// torn down outside shutdown(), so the index is monotonically growing
// during normal operation.
struct SlabRange {
    void* base;     // slab base address (inclusive)
    void* end;      // base + page_count * page_size (exclusive)
    Slab* slab;     // owning slab pointer
};

// Entry in the sorted large-object range index used by resolve_header() to
// map an interior pointer into a large object back to its allocation base
// (where the ObjectHeader lives). Parallel to `large_objects`; large objects
// have no per-slot structure, so range containment is the only way to recover
// the base from an interior pointer. Inserted on alloc_large, cleared on
// shutdown — never removed mid-run, because free_large keeps the vaddr mapped
// (tombstoned) until shutdown, mirroring the `large_objects` map's lifetime.
struct LargeRange {
    void* base;     // allocation base (inclusive) — the header location
    void* end;      // base + page_count * page_size (exclusive)
};

// Main slab allocator
// Provides memory allocation with tombstoning support for weak references
struct SlabAllocator {
    // Size class configuration
    // Each size class handles allocations up to its slot size
    static constexpr u32 NUM_SIZE_CLASSES = 8;
    static constexpr u32 SIZE_CLASS_SLOTS[NUM_SIZE_CLASSES] = {
        32, 64, 128, 256, 512, 1024, 2048, 4096
    };

    // Slabs for each size class
    Vector<UniquePtr<Slab>> size_classes[NUM_SIZE_CLASSES];

    // Flat index of all slab ranges across every size class, kept sorted
    // by base address. Populated by find_or_create_slab(), consulted by
    // find_slab_containing(). Inline base/end fields make binary search
    // cache-friendly compared to chasing the per-class UniquePtr<Slab>s.
    Vector<SlabRange> sorted_slabs;

    // Large object tracking (> 4KB)
    // Maps pointer to page count + tombstone state for deallocation
    tsl::robin_map<void*, LargeObjectInfo> large_objects;

    // Sorted (by base) index of large-object address ranges, enabling
    // resolve_header() to recover the base from an interior pointer. See
    // LargeRange. Kept in lockstep with large_objects: insert on alloc_large,
    // clear on shutdown.
    Vector<LargeRange> sorted_large;

    // Random generation for weak references
    RandomGen rng;

    // System page size (cached)
    u64 m_page_size;

    // Statistics
    u64 total_allocated;
    u64 total_tombstoned;

    SlabAllocator();
    ~SlabAllocator();

    // Initialize the allocator
    bool init();

    // Shutdown and release all memory
    void shutdown();

    // Allocate memory of the given size
    // Returns pointer to usable memory (after any header space the caller adds)
    // out_generation receives a random 64-bit generation for weak reference tracking
    void* alloc(u32 size, u64* out_generation);

    // Free previously allocated memory
    // Sets a new generation to invalidate weak references
    void free(void* ptr);

    // Check if a pointer was allocated by this allocator
    bool owns(void* ptr) const;

    // Resolve an interior pointer (any address within a live or tombstoned
    // allocation) to the owning object's ObjectHeader. Used by the
    // constraint-reference machinery to find the ref_count/generation behind
    // a `borrowed`-subscript or `[ref self]` borrow that targets an inline
    // field of a heap object. Returns nullptr for a pointer this allocator
    // does not own. A pointer to the allocation base resolves to itself.
    roxy_object_header* resolve_header(void* interior_ptr);

    // Scan all slabs and reclaim fully tombstoned ones
    // Calls remap_to_zero() on slabs where all slots are tombstoned
    // Returns number of pages reclaimed
    u32 reclaim_tombstoned();

private:
    // Get size class index for a given size, or NUM_SIZE_CLASSES if too large
    u32 size_to_class(u32 size) const;

    // Find or create a slab with free slots for the given size class
    Slab* find_or_create_slab(u32 class_idx);

    // Allocate from a slab
    void* alloc_from_slab(Slab* slab, u64* out_generation);

    // Allocate a large object (multiple pages)
    void* alloc_large(u32 size, u64* out_generation);

    // Free an object in a slab
    void free_in_slab(Slab* slab, u32 slot_idx);

    // Free a large object
    void free_large(void* ptr);

    // Find which slab contains a pointer, returns nullptr if not found
    Slab* find_slab_containing(void* ptr);
    const Slab* find_slab_containing(void* ptr) const;
};

// Build a `roxy_allocator` vtable that routes through the given slab. The
// returned vtable's `userdata` is the `SlabAllocator*`; the slab must
// outlive any allocations performed through the vtable.
roxy_allocator make_slab_allocator_vtable(SlabAllocator* slab);

}

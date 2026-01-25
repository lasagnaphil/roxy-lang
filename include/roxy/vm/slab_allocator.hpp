#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/tsl/robin_map.h"

namespace rx {

// State of a slot within a slab
enum class SlotState : u8 {
    FREE,       // Available for allocation
    ALIVE,      // Currently in use
    TOMBSTONE,  // Freed but not recyclable (weak refs may exist)
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
    Vector<SlotState> states;   // Per-slot state

    // Get pointer to a specific slot
    void* slot_ptr(u32 index) const {
        return reinterpret_cast<u8*>(base_addr) + static_cast<u64>(index) * slot_size;
    }

    // Check if all slots are tombstoned (can be remapped to zeros)
    bool all_tombstoned() const {
        return live_count == 0 && free_head == 0xFFFFFFFF;
    }
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
    Vector<Slab*> size_classes[NUM_SIZE_CLASSES];

    // Large object tracking (> 4KB)
    // Maps pointer to page count for deallocation
    tsl::robin_map<void*, u32> large_objects;

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
    Slab* find_slab_containing(void* ptr) const;
};

}

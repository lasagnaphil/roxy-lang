#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Platform-agnostic virtual memory operations
// Used by the slab allocator to manage memory pages
struct VirtualMemoryOps {
    // Reserve address space without committing physical memory
    // Returns nullptr on failure
    static void* reserve(u64 size);

    // Commit physical memory to reserved range
    // Returns true on success
    static bool commit(void* addr, u64 size);

    // Decommit physical memory (keep reservation)
    // Returns true on success
    static bool decommit(void* addr, u64 size);

    // Release address space entirely
    static void release(void* addr, u64 size);

    // Zero the page and make it read-only (for tombstoned memory)
    // This allows safe reads from freed memory (returns zeros)
    // Returns true on success
    static bool remap_to_zero(void* addr, u64 size);

    // Get system page size (typically 4096)
    static u64 page_size();
};

}

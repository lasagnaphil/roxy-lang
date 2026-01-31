#include "roxy/vm/slab_allocator.hpp"
#include "roxy/vm/vmem.hpp"

#include <cstring>
#include <cassert>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#include <unistd.h>
#endif

namespace rx {

// RandomGen implementation

u64 RandomGen::splitmix64(u64 x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

void RandomGen::seed(u64 s) {
    // Use SplitMix64 to initialize state from a single seed
    state[0] = splitmix64(s);
    state[1] = splitmix64(state[0]);

    // Ensure state is never all zeros
    if (state[0] == 0 && state[1] == 0) {
        state[0] = 0x123456789ABCDEFULL;
    }
}

u64 RandomGen::next() {
    // xorshift128+ algorithm
    u64 s1 = state[0];
    u64 s0 = state[1];
    u64 result = s0 + s1;
    state[0] = s0;
    s1 ^= s1 << 23;
    state[1] = s1 ^ s0 ^ (s1 >> 18) ^ (s0 >> 5);
    return result;
}

// SlabAllocator implementation

SlabAllocator::SlabAllocator()
    : m_page_size(0)
    , total_allocated(0)
    , total_tombstoned(0)
{
    rng.state[0] = 0;
    rng.state[1] = 0;
}

SlabAllocator::~SlabAllocator() {
    shutdown();
}

bool SlabAllocator::init() {
    m_page_size = VirtualMemoryOps::page_size();
    if (m_page_size == 0) {
        return false;
    }

    // Seed the RNG with high-resolution timer + process ID
    u64 seed = 0;

#ifdef _WIN32
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    seed = static_cast<u64>(qpc.QuadPart);
    seed ^= static_cast<u64>(GetCurrentProcessId()) << 32;
    seed ^= static_cast<u64>(GetTickCount64());
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    seed = static_cast<u64>(ts.tv_sec) * 1000000000ULL + static_cast<u64>(ts.tv_nsec);
    seed ^= static_cast<u64>(getpid()) << 32;
#endif

    rng.seed(seed);

    total_allocated = 0;
    total_tombstoned = 0;

    return true;
}

void SlabAllocator::shutdown() {
    // Free all slabs
    for (u32 i = 0; i < NUM_SIZE_CLASSES; i++) {
        for (auto& slab : size_classes[i]) {
            if (slab->base_addr) {
                VirtualMemoryOps::release(slab->base_addr,
                    static_cast<u64>(slab->page_count) * m_page_size);
            }
            // UniquePtr automatically deletes the Slab
        }
        size_classes[i].clear();
    }

    // Free large objects
    for (auto& [ptr, page_count] : large_objects) {
        VirtualMemoryOps::release(ptr, static_cast<u64>(page_count) * m_page_size);
    }
    large_objects.clear();

    total_allocated = 0;
    total_tombstoned = 0;
}

u32 SlabAllocator::size_to_class(u32 size) const {
    for (u32 i = 0; i < NUM_SIZE_CLASSES; i++) {
        if (size <= SIZE_CLASS_SLOTS[i]) {
            return i;
        }
    }
    return NUM_SIZE_CLASSES;  // Large object
}

Slab* SlabAllocator::find_or_create_slab(u32 class_idx) {
    assert(class_idx < NUM_SIZE_CLASSES);

    // Try to find an existing slab with free slots
    for (auto& slab : size_classes[class_idx]) {
        if (slab->free_head != 0xFFFFFFFF) {
            return slab.get();
        }
    }

    // Create a new slab
    u32 slot_size = SIZE_CLASS_SLOTS[class_idx];

    // Determine slab size: at least 1 page, enough for ~64 slots minimum
    u64 min_slab_size = static_cast<u64>(slot_size) * 64;
    u32 page_count = static_cast<u32>((min_slab_size + m_page_size - 1) / m_page_size);
    if (page_count < 1) page_count = 1;

    u64 slab_size = static_cast<u64>(page_count) * m_page_size;

    // Reserve and commit memory
    void* mem = VirtualMemoryOps::reserve(slab_size);
    if (mem == nullptr) {
        return nullptr;
    }

    if (!VirtualMemoryOps::commit(mem, slab_size)) {
        VirtualMemoryOps::release(mem, slab_size);
        return nullptr;
    }

    // Create slab structure
    auto slab = make_unique<Slab>();
    slab->base_addr = mem;
    slab->page_count = page_count;
    slab->slot_size = slot_size;
    slab->slot_count = static_cast<u32>(slab_size / slot_size);
    slab->free_head = 0;
    slab->live_count = 0;
    slab->remapped = false;

    // Initialize slot states and free list
    slab->states.resize(slab->slot_count);
    for (u32 i = 0; i < slab->slot_count; i++) {
        slab->states[i] = SlotState::FREE;
        // Store next free index in first bytes of slot (intrusive free list)
        u32* slot = reinterpret_cast<u32*>(slab->slot_ptr(i));
        *slot = (i + 1 < slab->slot_count) ? (i + 1) : 0xFFFFFFFF;
    }

    Slab* result = slab.get();
    size_classes[class_idx].push_back(std::move(slab));
    return result;
}

void* SlabAllocator::alloc_from_slab(Slab* slab, u64* out_generation) {
    assert(slab->free_head != 0xFFFFFFFF);

    // Pop from free list
    u32 slot_idx = slab->free_head;
    void* slot = slab->slot_ptr(slot_idx);
    slab->free_head = *reinterpret_cast<u32*>(slot);

    // Update state
    slab->states[slot_idx] = SlotState::ALIVE;
    slab->live_count++;
    total_allocated++;

    // Generate random generation
    *out_generation = rng.next();

    // Zero the slot
    std::memset(slot, 0, slab->slot_size);

    return slot;
}

void* SlabAllocator::alloc_large(u32 size, u64* out_generation) {
    // Round up to page boundary
    u32 page_count = static_cast<u32>((size + m_page_size - 1) / m_page_size);
    u64 alloc_size = static_cast<u64>(page_count) * m_page_size;

    // Reserve and commit
    void* mem = VirtualMemoryOps::reserve(alloc_size);
    if (mem == nullptr) {
        return nullptr;
    }

    if (!VirtualMemoryOps::commit(mem, alloc_size)) {
        VirtualMemoryOps::release(mem, alloc_size);
        return nullptr;
    }

    // Track the allocation
    large_objects[mem] = page_count;
    total_allocated++;

    // Generate random generation
    *out_generation = rng.next();

    // Zero the memory
    std::memset(mem, 0, size);

    return mem;
}

void* SlabAllocator::alloc(u32 size, u64* out_generation) {
    u32 class_idx = size_to_class(size);

    if (class_idx < NUM_SIZE_CLASSES) {
        Slab* slab = find_or_create_slab(class_idx);
        if (slab == nullptr) {
            return nullptr;
        }
        return alloc_from_slab(slab, out_generation);
    } else {
        return alloc_large(size, out_generation);
    }
}

Slab* SlabAllocator::find_slab_containing(void* ptr) {
    for (u32 i = 0; i < NUM_SIZE_CLASSES; i++) {
        for (auto& slab : size_classes[i]) {
            u8* base = reinterpret_cast<u8*>(slab->base_addr);
            u8* end = base + static_cast<u64>(slab->page_count) * m_page_size;
            u8* p = reinterpret_cast<u8*>(ptr);

            if (p >= base && p < end) {
                return slab.get();
            }
        }
    }
    return nullptr;
}

const Slab* SlabAllocator::find_slab_containing(void* ptr) const {
    for (u32 i = 0; i < NUM_SIZE_CLASSES; i++) {
        for (const auto& slab : size_classes[i]) {
            const u8* base = reinterpret_cast<const u8*>(slab->base_addr);
            const u8* end = base + static_cast<u64>(slab->page_count) * m_page_size;
            const u8* p = reinterpret_cast<const u8*>(ptr);

            if (p >= base && p < end) {
                return slab.get();
            }
        }
    }
    return nullptr;
}

void SlabAllocator::free_in_slab(Slab* slab, u32 slot_idx) {
    assert(slot_idx < slab->slot_count);
    assert(slab->states[slot_idx] == SlotState::ALIVE);

    // Mark as tombstone (not reusable - weak refs might still point here)
    slab->states[slot_idx] = SlotState::TOMBSTONE;
    slab->live_count--;
    total_tombstoned++;

    // Note: We do NOT add the slot back to the free list
    // This ensures weak references can safely read zeros from tombstoned memory

    // If all slots are now tombstoned, we could remap the entire slab to zeros
    // For now, we leave the memory as-is (zeroed during alloc)
    // In a more advanced implementation, we'd track tombstoned slabs
    // and periodically remap them when memory pressure is high
}

void SlabAllocator::free_large(void* ptr) {
    auto it = large_objects.find(ptr);
    if (it == large_objects.end()) {
        return;  // Not a large object (or already freed)
    }

    u32 page_count = it->second;
    u64 size = static_cast<u64>(page_count) * m_page_size;

    // Zero the memory and make it read-only (tombstone behavior)
    VirtualMemoryOps::remap_to_zero(ptr, size);

    // Remove from tracking (but don't release - keep mapped for weak refs)
    // In a production system, we might want to track these separately
    // and release them when memory pressure is high
    large_objects.erase(it);
    total_tombstoned++;
}

void SlabAllocator::free(void* ptr) {
    if (ptr == nullptr) {
        return;
    }

    // Check if it's in a slab
    Slab* slab = find_slab_containing(ptr);
    if (slab != nullptr) {
        // Calculate slot index
        u64 offset = reinterpret_cast<u8*>(ptr) - reinterpret_cast<u8*>(slab->base_addr);
        u32 slot_idx = static_cast<u32>(offset / slab->slot_size);
        free_in_slab(slab, slot_idx);
        return;
    }

    // Check if it's a large object
    if (large_objects.find(ptr) != large_objects.end()) {
        free_large(ptr);
        return;
    }

    // Unknown pointer - this is a bug in the caller
    assert(false && "SlabAllocator::free called with unknown pointer");
}

bool SlabAllocator::owns(void* ptr) const {
    if (ptr == nullptr) {
        return false;
    }

    // Check slabs
    if (find_slab_containing(ptr) != nullptr) {
        return true;
    }

    // Check large objects
    return large_objects.find(ptr) != large_objects.end();
}

u32 SlabAllocator::reclaim_tombstoned() {
    u32 total_reclaimed = 0;

    for (u32 i = 0; i < NUM_SIZE_CLASSES; i++) {
        for (auto& slab : size_classes[i]) {
            // Skip already reclaimed slabs
            if (slab->remapped) {
                continue;
            }

            // Check if all slots are tombstoned
            if (slab->all_tombstoned()) {
                // Remap the slab memory to zeros, releasing physical memory
                u64 slab_size = static_cast<u64>(slab->page_count) * m_page_size;
                VirtualMemoryOps::remap_to_zero(slab->base_addr, slab_size);
                slab->remapped = true;
                total_reclaimed += slab->page_count;
            }
        }
    }

    return total_reclaimed;
}

}

#include "roxy/rt/slab_allocator.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/rt/vmem.hpp"

#include <algorithm>
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

// Free-list link for a recycled slot. Stored at offset sizeof(ObjectHeader)
// rather than offset 0, because offset 0..7 holds weak_generation: that
// field MUST read as zero while the slot sits on the free list so weak
// refs see is_alive() == false during the gap between free and re-alloc.
// Every size class has slot_size >= 32, so we always have room past the
// 16-byte header for a u32 next-index. alloc_from_slab() zeroes the slot
// before returning, clearing this scratch area before the user header
// is written.
static constexpr u32 FREE_LIST_OFFSET = sizeof(ObjectHeader);

static inline u32* slot_next_link(void* slot) {
    return reinterpret_cast<u32*>(static_cast<u8*>(slot) + FREE_LIST_OFFSET);
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

    // Drop the sorted slab-range index — its entries point into Slab
    // structs that the per-class UniquePtrs above just released.
    sorted_slabs.clear();

    // Free large objects (both live and tombstoned)
    for (auto& [ptr, info] : large_objects) {
        VirtualMemoryOps::release(ptr, static_cast<u64>(info.page_count) * m_page_size);
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

    // Initialize slot states and free list. Fresh vmem is zeroed by the
    // OS, so weak_generation at offset 0 is already 0; we only need to
    // write the next-link past the header.
    slab->states.resize(slab->slot_count);
    for (u32 i = 0; i < slab->slot_count; i++) {
        slab->states[i] = SlotState::FREE;
        *slot_next_link(slab->slot_ptr(i)) =
            (i + 1 < slab->slot_count) ? (i + 1) : 0xFFFFFFFF;
    }

    Slab* result = slab.get();
    size_classes[class_idx].push_back(std::move(slab));

    // Register the new range in the sorted index. Slab vmem reservations
    // come back at arbitrary addresses, so we have to insert in order
    // rather than appending. Insertion is O(N) for the shift but slab
    // creation is rare relative to alloc/free traffic.
    SlabRange range;
    range.base = result->base_addr;
    range.end = reinterpret_cast<u8*>(result->base_addr) + slab_size;
    range.slab = result;
    auto* insert_pos = std::upper_bound(
        sorted_slabs.begin(), sorted_slabs.end(), range.base,
        [](void* p, const SlabRange& r) { return p < r.base; });
    sorted_slabs.insert(insert_pos, range);

    return result;
}

void* SlabAllocator::alloc_from_slab(Slab* slab, u64* out_generation) {
    assert(slab->free_head != 0xFFFFFFFF);

    // Pop from free list
    u32 slot_idx = slab->free_head;
    void* slot = slab->slot_ptr(slot_idx);
    slab->free_head = *slot_next_link(slot);

    // Update state
    assert(slab->states[slot_idx] == SlotState::FREE);
    slab->states[slot_idx] = SlotState::ALIVE;
    slab->live_count++;
    total_allocated++;

    // Generate random generation (must be non-zero; 0 means dead)
    *out_generation = rng.next();
    if (*out_generation == 0) *out_generation = rng.next();

    // Zero the slot, including the recycled next-link past the header,
    // so the freshly returned object starts from a clean state.
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
    LargeObjectInfo info;
    info.page_count = page_count;
    info.tombstoned = 0;
    large_objects[mem] = info;
    total_allocated++;

    // Generate random generation (must be non-zero; 0 means dead)
    *out_generation = rng.next();
    if (*out_generation == 0) *out_generation = rng.next();

    // Zero the full page-aligned allocation, not just the caller-requested
    // size. Matches the slab path which zeros slot_size (not request size),
    // and prevents the padding between `size` and `alloc_size` from leaking
    // stale committed memory into user-visible reads.
    std::memset(mem, 0, alloc_size);

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

// Binary search the sorted slab-range index. Slab address ranges never
// overlap, so upper_bound on the input pointer either returns
// sorted_slabs.begin() (no candidate — pointer falls before every range)
// or points past the unique candidate range, which is the entry just
// before. A final half-open bound check confirms membership.
Slab* SlabAllocator::find_slab_containing(void* ptr) {
    if (sorted_slabs.empty()) return nullptr;

    auto* it = std::upper_bound(
        sorted_slabs.begin(), sorted_slabs.end(), ptr,
        [](void* p, const SlabRange& r) { return p < r.base; });
    if (it == sorted_slabs.begin()) return nullptr;
    --it;
    return (ptr < it->end) ? it->slab : nullptr;
}

const Slab* SlabAllocator::find_slab_containing(void* ptr) const {
    if (sorted_slabs.empty()) return nullptr;

    auto* it = std::upper_bound(
        sorted_slabs.begin(), sorted_slabs.end(), ptr,
        [](void* p, const SlabRange& r) { return p < r.base; });
    if (it == sorted_slabs.begin()) return nullptr;
    --it;
    return (ptr < it->end) ? it->slab : nullptr;
}

void SlabAllocator::free_in_slab(Slab* slab, u32 slot_idx) {
    assert(slot_idx < slab->slot_count);
    // Catches double-free at this layer (a recycled-then-realloc'd slot
    // would be ALIVE again, so this fires only if the same generation
    // is freed twice without an intervening alloc — i.e., true
    // double-free, not stale-pointer use-after-free).
    assert(slab->states[slot_idx] == SlotState::ALIVE);

    // Zero the entire slot. weak_generation at offset 0 is now 0, so
    // any outstanding weak ref reads is_alive() == false until and
    // unless the slot is re-allocated and stamped with a new gen.
    void* slot = slab->slot_ptr(slot_idx);
    std::memset(slot, 0, slab->slot_size);

    // Push the slot back onto the intrusive free list. Recycling here
    // is what closes the fragmentation hole — without it, mixed-lifetime
    // workloads accumulate dead slots that never become reusable until
    // every other slot in the slab also dies. Safety against stale weak
    // refs is provided by the 64-bit random weak_generation: a fresh
    // alloc into this slot writes a new gen that won't match a cached
    // gen except by 2^-64 collision. The next-link sits past the header
    // so weak_generation stays zero while the slot is parked here.
    *slot_next_link(slot) = slab->free_head;
    slab->free_head = slot_idx;
    slab->states[slot_idx] = SlotState::FREE;
    slab->live_count--;
    total_tombstoned++;
}

void SlabAllocator::free_large(void* ptr) {
    auto it = large_objects.find(ptr);
    if (it == large_objects.end()) {
        return;  // Not a large object
    }

    if (it->second.tombstoned) {
        return;  // Already freed (idempotent double-free)
    }

    u64 size = static_cast<u64>(it->second.page_count) * m_page_size;

    // Zero the memory and make it read-only (tombstone behavior)
    // Keep mapped so weak refs can safely read zeros
    VirtualMemoryOps::remap_to_zero(ptr, size);

    // Mark as tombstoned but keep tracked — shutdown() will release the vaddr
    it.value().tombstoned = 1;
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

            // A drained slab (live_count == 0) may still have FREE slots
            // reachable from the free list — that was the whole point of
            // recycling. Reclaiming releases their physical memory; we
            // also have to disconnect the slab from allocation rotation,
            // since alloc_from_slab would otherwise hand out slots that
            // we just remapped. Setting free_head to 0xFFFFFFFF makes
            // find_or_create_slab skip the slab; the per-slot states are
            // updated for consistency (debug assertions and future
            // diagnostic walks). The remapped vaddr stays mapped to
            // zeros so weak refs continue to read is_alive() == false.
            if (slab->is_drained()) {
                u64 slab_size = static_cast<u64>(slab->page_count) * m_page_size;
                VirtualMemoryOps::remap_to_zero(slab->base_addr, slab_size);
                slab->remapped = true;
                slab->free_head = 0xFFFFFFFF;
                for (u32 s = 0; s < slab->slot_count; s++) {
                    slab->states[s] = SlotState::FREE;
                }
                total_reclaimed += slab->page_count;
            }
        }
    }

    return total_reclaimed;
}

// ===== roxy_allocator vtable adapter =====
//
// Wraps the slab in the C `roxy_allocator` shape so it can be plugged into
// `roxy_ctx.allocator`. The slab returns raw memory; the runtime caller
// (e.g. `roxy_alloc`) writes the `roxy_object_header` itself, matching the
// existing slab + ObjectHeader contract.

static void* slab_alloc_fn(void* userdata, uint32_t total_size,
                           uint64_t* out_generation) {
    auto* slab = static_cast<SlabAllocator*>(userdata);
    u64 gen = 0;
    void* mem = slab->alloc(total_size, &gen);
    if (out_generation) *out_generation = gen;
    return mem;
}

static void slab_free_fn(void* userdata, void* header_ptr) {
    auto* slab = static_cast<SlabAllocator*>(userdata);
    if (!header_ptr) return;
    // The slab zeroes the slot on free, which already tombstones
    // `weak_generation`. No additional bookkeeping needed here.
    slab->free(header_ptr);
}

static bool slab_owns_fn(void* userdata, void* ptr) {
    auto* slab = static_cast<SlabAllocator*>(userdata);
    return slab->owns(ptr);
}

roxy_allocator make_slab_allocator_vtable(SlabAllocator* slab) {
    roxy_allocator vtable;
    vtable.alloc = slab_alloc_fn;
    vtable.free = slab_free_fn;
    vtable.owns = slab_owns_fn;
    vtable.userdata = slab;
    return vtable;
}

}

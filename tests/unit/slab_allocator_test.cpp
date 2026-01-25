#include "roxy/core/doctest/doctest.h"
#include "roxy/vm/slab_allocator.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/vm.hpp"

#include <random>
#include <algorithm>
#include <array>
#include <vector>
#include <cstring>

using namespace rx;

// ============================================================================
// Slab Allocator Unit Tests
// ============================================================================

TEST_CASE("Unit - SlabAllocator basic allocation") {
    SlabAllocator allocator;
    CHECK(allocator.init());

    // Allocate some small objects
    u64 gen1, gen2, gen3;
    void* ptr1 = allocator.alloc(32, &gen1);
    void* ptr2 = allocator.alloc(32, &gen2);
    void* ptr3 = allocator.alloc(64, &gen3);

    CHECK(ptr1 != nullptr);
    CHECK(ptr2 != nullptr);
    CHECK(ptr3 != nullptr);

    // All should have different generations (random)
    CHECK(gen1 != gen2);
    CHECK(gen2 != gen3);

    // Clean up
    allocator.free(ptr1);
    allocator.free(ptr2);
    allocator.free(ptr3);
    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator size classes") {
    SlabAllocator allocator;
    CHECK(allocator.init());

    // Allocate objects of various sizes
    u64 gen;
    void* ptr32 = allocator.alloc(32, &gen);
    void* ptr64 = allocator.alloc(64, &gen);
    void* ptr128 = allocator.alloc(128, &gen);
    void* ptr256 = allocator.alloc(256, &gen);
    void* ptr512 = allocator.alloc(512, &gen);
    void* ptr1024 = allocator.alloc(1024, &gen);
    void* ptr2048 = allocator.alloc(2048, &gen);
    void* ptr4096 = allocator.alloc(4096, &gen);

    CHECK(ptr32 != nullptr);
    CHECK(ptr64 != nullptr);
    CHECK(ptr128 != nullptr);
    CHECK(ptr256 != nullptr);
    CHECK(ptr512 != nullptr);
    CHECK(ptr1024 != nullptr);
    CHECK(ptr2048 != nullptr);
    CHECK(ptr4096 != nullptr);

    allocator.free(ptr32);
    allocator.free(ptr64);
    allocator.free(ptr128);
    allocator.free(ptr256);
    allocator.free(ptr512);
    allocator.free(ptr1024);
    allocator.free(ptr2048);
    allocator.free(ptr4096);
    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator large object") {
    SlabAllocator allocator;
    CHECK(allocator.init());

    // Allocate a large object (> 4096 bytes)
    u64 gen;
    void* ptr = allocator.alloc(8192, &gen);
    CHECK(ptr != nullptr);

    allocator.free(ptr);
    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator random generation uniqueness") {
    SlabAllocator allocator;
    CHECK(allocator.init());

    // Allocate many objects and verify generation uniqueness
    constexpr u32 NUM_ALLOCS = 100;
    u64 generations[NUM_ALLOCS];
    void* pointers[NUM_ALLOCS];

    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        pointers[i] = allocator.alloc(32, &generations[i]);
        CHECK(pointers[i] != nullptr);
    }

    // Check that generations are unique (birthday paradox: with 64-bit random,
    // 100 samples should have no collisions with overwhelming probability)
    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        for (u32 j = i + 1; j < NUM_ALLOCS; j++) {
            CHECK(generations[i] != generations[j]);
        }
    }

    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        allocator.free(pointers[i]);
    }
    allocator.shutdown();
}

// ============================================================================
// Weak Reference Unit Tests
// ============================================================================

TEST_CASE("Unit - Weak reference creation and validation") {
    RoxyVM vm;
    CHECK(vm_init(&vm, VMConfig()));

    // Allocate an object
    u32 type_id = register_object_type("TestObject", 16, nullptr);
    void* data = object_alloc(&vm, type_id, 16);
    CHECK(data != nullptr);

    // Create a weak reference
    u64 gen = weak_ref_create(data);
    CHECK(gen != 0);

    // Verify the weak reference is valid
    CHECK(weak_ref_valid(data, gen));

    // Clean up
    object_free(&vm, data);
    vm_destroy(&vm);
}

TEST_CASE("Unit - Weak reference invalidation after free") {
    RoxyVM vm;
    CHECK(vm_init(&vm, VMConfig()));

    // Allocate an object
    u32 type_id = register_object_type("TestObject2", 16, nullptr);
    void* data = object_alloc(&vm, type_id, 16);
    CHECK(data != nullptr);

    // Create a weak reference
    u64 gen = weak_ref_create(data);
    CHECK(weak_ref_valid(data, gen));

    // Free the object
    object_free(&vm, data);

    // The weak reference should now be invalid
    // (memory is tombstoned - reads zeros, so is_alive() returns false)
    CHECK(weak_ref_valid(data, gen) == false);

    vm_destroy(&vm);
}

TEST_CASE("Unit - ObjectHeader flags") {
    RoxyVM vm;
    CHECK(vm_init(&vm, VMConfig()));

    u32 type_id = register_object_type("TestObject3", 16, nullptr);
    void* data = object_alloc(&vm, type_id, 16);
    CHECK(data != nullptr);

    ObjectHeader* header = get_header_from_data(data);

    // Object should be alive after allocation
    CHECK(header->is_alive());
    CHECK((header->flags & ObjectHeader::FLAG_ALIVE) != 0);

    // Free the object
    object_free(&vm, data);

    // Note: After free, the header memory is zeroed by tombstoning,
    // so is_alive() should return false (flags == 0)
    // We can't check the header directly after free as it's been tombstoned

    vm_destroy(&vm);
}

TEST_CASE("Unit - Multiple allocations and frees") {
    RoxyVM vm;
    CHECK(vm_init(&vm, VMConfig()));

    u32 type_id = register_object_type("TestObject4", 32, nullptr);

    // Perform many allocations and frees
    for (int iteration = 0; iteration < 10; iteration++) {
        void* ptrs[10];
        u64 gens[10];

        // Allocate
        for (int i = 0; i < 10; i++) {
            ptrs[i] = object_alloc(&vm, type_id, 32);
            CHECK(ptrs[i] != nullptr);
            gens[i] = weak_ref_create(ptrs[i]);
        }

        // Verify all are valid
        for (int i = 0; i < 10; i++) {
            CHECK(weak_ref_valid(ptrs[i], gens[i]));
        }

        // Free all
        for (int i = 0; i < 10; i++) {
            object_free(&vm, ptrs[i]);
        }

        // All weak refs should now be invalid
        for (int i = 0; i < 10; i++) {
            CHECK(weak_ref_valid(ptrs[i], gens[i]) == false);
        }
    }

    vm_destroy(&vm);
}

// ============================================================================
// Slab Allocator Stress Tests
// ============================================================================

TEST_CASE("Stress - SlabAllocator heavy allocation pattern") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    constexpr u32 NUM_OBJECTS = 10000;
    struct AllocInfo {
        void* ptr;
        u64 generation;
        u32 size;
        u32 magic;  // Pattern to verify memory integrity
    };

    std::vector<AllocInfo> allocs;
    allocs.reserve(NUM_OBJECTS);

    // Phase 1: Allocate many objects with varying sizes
    std::mt19937 rng(42);  // Fixed seed for reproducibility
    std::uniform_int_distribution<u32> size_dist(1, 4096);

    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        AllocInfo info;
        info.size = size_dist(rng);
        info.magic = 0xDEADBEEF ^ i;
        info.ptr = allocator.alloc(info.size, &info.generation);

        REQUIRE(info.ptr != nullptr);
        REQUIRE(info.generation != 0);

        // Write magic pattern to memory
        u32* data = static_cast<u32*>(info.ptr);
        *data = info.magic;

        allocs.push_back(info);
    }

    // Phase 2: Verify all allocations are intact
    for (const auto& info : allocs) {
        u32* data = static_cast<u32*>(info.ptr);
        CHECK(*data == info.magic);
    }

    // Phase 3: Free all objects
    for (const auto& info : allocs) {
        allocator.free(info.ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator interleaved alloc/free") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    constexpr u32 ITERATIONS = 5000;
    constexpr u32 MAX_LIVE = 500;

    struct AllocInfo {
        void* ptr;
        u64 generation;
        u32 size;
        u64 pattern;
    };

    std::vector<AllocInfo> live_allocs;
    live_allocs.reserve(MAX_LIVE);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<u32> size_dist(8, 2048);
    std::uniform_int_distribution<u32> action_dist(0, 99);

    for (u32 iter = 0; iter < ITERATIONS; iter++) {
        u32 action = action_dist(rng);

        // 60% chance to allocate if under limit, 40% chance to free
        bool should_alloc = (action < 60) && (live_allocs.size() < MAX_LIVE);
        bool should_free = (action >= 60) && !live_allocs.empty();

        if (live_allocs.empty()) should_alloc = true;
        if (live_allocs.size() >= MAX_LIVE) should_free = true;

        if (should_alloc) {
            AllocInfo info;
            info.size = size_dist(rng);
            info.pattern = rng();
            info.ptr = allocator.alloc(info.size, &info.generation);

            REQUIRE(info.ptr != nullptr);

            // Write pattern
            if (info.size >= sizeof(u64)) {
                *static_cast<u64*>(info.ptr) = info.pattern;
            }

            live_allocs.push_back(info);
        } else if (should_free) {
            // Pick a random allocation to free
            std::uniform_int_distribution<size_t> idx_dist(0, live_allocs.size() - 1);
            size_t idx = idx_dist(rng);

            AllocInfo& info = live_allocs[idx];

            // Verify pattern before freeing
            if (info.size >= sizeof(u64)) {
                CHECK(*static_cast<u64*>(info.ptr) == info.pattern);
            }

            allocator.free(info.ptr);

            // Remove from live list
            live_allocs[idx] = live_allocs.back();
            live_allocs.pop_back();
        }
    }

    // Clean up remaining allocations
    for (const auto& info : live_allocs) {
        allocator.free(info.ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator all size classes simultaneously") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate many objects in each size class
    constexpr u32 OBJECTS_PER_CLASS = 500;
    constexpr u32 SIZE_CLASSES[] = {32, 64, 128, 256, 512, 1024, 2048, 4096};
    constexpr u32 NUM_CLASSES = sizeof(SIZE_CLASSES) / sizeof(SIZE_CLASSES[0]);

    struct AllocInfo {
        void* ptr;
        u64 generation;
        u32 size_class_idx;
        u32 magic;
    };

    std::vector<AllocInfo> all_allocs;
    all_allocs.reserve(OBJECTS_PER_CLASS * NUM_CLASSES);

    // Allocate in round-robin across size classes
    for (u32 i = 0; i < OBJECTS_PER_CLASS; i++) {
        for (u32 sc = 0; sc < NUM_CLASSES; sc++) {
            AllocInfo info;
            info.size_class_idx = sc;
            info.magic = (i << 16) | sc;
            info.ptr = allocator.alloc(SIZE_CLASSES[sc], &info.generation);

            REQUIRE(info.ptr != nullptr);

            // Write magic
            *static_cast<u32*>(info.ptr) = info.magic;

            all_allocs.push_back(info);
        }
    }

    // Shuffle the allocations
    std::mt19937 rng(999);
    std::shuffle(all_allocs.begin(), all_allocs.end(), rng);

    // Verify all magics
    for (const auto& info : all_allocs) {
        CHECK(*static_cast<u32*>(info.ptr) == info.magic);
    }

    // Free in shuffled order
    for (const auto& info : all_allocs) {
        allocator.free(info.ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator generation uniqueness under heavy load") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    constexpr u32 NUM_GENERATIONS = 10000;
    std::vector<u64> generations;
    generations.reserve(NUM_GENERATIONS);

    // Collect many generations
    for (u32 i = 0; i < NUM_GENERATIONS; i++) {
        u64 gen;
        void* ptr = allocator.alloc(32, &gen);
        REQUIRE(ptr != nullptr);
        generations.push_back(gen);
        allocator.free(ptr);
    }

    // Sort and check for duplicates
    std::sort(generations.begin(), generations.end());
    for (size_t i = 1; i < generations.size(); i++) {
        CHECK(generations[i] != generations[i-1]);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator memory zeroing verification") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    constexpr u32 NUM_ITERATIONS = 100;
    constexpr u32 ALLOC_SIZE = 256;

    for (u32 iter = 0; iter < NUM_ITERATIONS; iter++) {
        u64 gen;
        void* ptr = allocator.alloc(ALLOC_SIZE, &gen);
        REQUIRE(ptr != nullptr);

        // Verify memory is zeroed on allocation
        u8* bytes = static_cast<u8*>(ptr);
        for (u32 i = 0; i < ALLOC_SIZE; i++) {
            REQUIRE(bytes[i] == 0);
        }

        // Fill with non-zero pattern
        std::memset(ptr, 0xAB, ALLOC_SIZE);

        // Free the object
        allocator.free(ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator large objects") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    constexpr u32 NUM_LARGE_OBJECTS = 50;
    constexpr u32 LARGE_SIZES[] = {8192, 16384, 32768, 65536, 131072};
    constexpr u32 NUM_SIZES = sizeof(LARGE_SIZES) / sizeof(LARGE_SIZES[0]);

    struct LargeAlloc {
        void* ptr;
        u64 generation;
        u32 size;
        u64 checksum;
    };

    std::vector<LargeAlloc> allocs;
    std::mt19937 rng(777);

    for (u32 i = 0; i < NUM_LARGE_OBJECTS; i++) {
        LargeAlloc info;
        info.size = LARGE_SIZES[i % NUM_SIZES];
        info.ptr = allocator.alloc(info.size, &info.generation);

        REQUIRE(info.ptr != nullptr);

        // Write a pattern and compute checksum
        u64* data = static_cast<u64*>(info.ptr);
        u32 num_u64s = info.size / sizeof(u64);
        info.checksum = 0;
        for (u32 j = 0; j < num_u64s; j++) {
            data[j] = rng();
            info.checksum ^= data[j];
        }

        allocs.push_back(info);
    }

    // Verify checksums
    for (const auto& info : allocs) {
        u64* data = static_cast<u64*>(info.ptr);
        u32 num_u64s = info.size / sizeof(u64);
        u64 checksum = 0;
        for (u32 j = 0; j < num_u64s; j++) {
            checksum ^= data[j];
        }
        CHECK(checksum == info.checksum);
    }

    // Free all
    for (const auto& info : allocs) {
        allocator.free(info.ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - Weak reference validation under heavy churn") {
    RoxyVM vm;
    REQUIRE(vm_init(&vm, VMConfig()));

    u32 type_id = register_object_type("ChurnObject", 64, nullptr);

    constexpr u32 NUM_OBJECTS = 1000;
    constexpr u32 NUM_ITERATIONS = 100;

    struct ObjInfo {
        void* ptr;
        u64 generation;
        bool is_alive;
    };

    std::vector<ObjInfo> objects(NUM_OBJECTS);
    std::mt19937 rng(54321);

    // Initial allocation
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        objects[i].ptr = object_alloc(&vm, type_id, 64);
        REQUIRE(objects[i].ptr != nullptr);
        objects[i].generation = weak_ref_create(objects[i].ptr);
        objects[i].is_alive = true;
    }

    // Churn: randomly free and reallocate
    for (u32 iter = 0; iter < NUM_ITERATIONS; iter++) {
        // Free ~10% of live objects
        for (u32 i = 0; i < NUM_OBJECTS; i++) {
            if (objects[i].is_alive && (rng() % 10 == 0)) {
                // Verify weak ref is valid before freeing
                CHECK(weak_ref_valid(objects[i].ptr, objects[i].generation));

                object_free(&vm, objects[i].ptr);
                objects[i].is_alive = false;
            }
        }

        // Verify all weak refs: alive should be valid, dead should be invalid
        for (u32 i = 0; i < NUM_OBJECTS; i++) {
            bool is_valid = weak_ref_valid(objects[i].ptr, objects[i].generation);
            if (objects[i].is_alive) {
                CHECK(is_valid);
            } else {
                CHECK(!is_valid);
            }
        }

        // Reallocate dead objects
        for (u32 i = 0; i < NUM_OBJECTS; i++) {
            if (!objects[i].is_alive) {
                objects[i].ptr = object_alloc(&vm, type_id, 64);
                REQUIRE(objects[i].ptr != nullptr);
                objects[i].generation = weak_ref_create(objects[i].ptr);
                objects[i].is_alive = true;
            }
        }
    }

    // Clean up
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        if (objects[i].is_alive) {
            object_free(&vm, objects[i].ptr);
        }
    }

    vm_destroy(&vm);
}

TEST_CASE("Stress - SlabAllocator pointer alignment") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Verify all allocations are properly aligned
    constexpr u32 NUM_ALLOCS = 1000;
    std::mt19937 rng(11111);
    std::uniform_int_distribution<u32> size_dist(1, 4096);

    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        u32 size = size_dist(rng);
        u64 gen;
        void* ptr = allocator.alloc(size, &gen);

        REQUIRE(ptr != nullptr);

        // Check alignment (should be at least 8-byte aligned)
        uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
        CHECK((addr % 8) == 0);

        allocator.free(ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - SlabAllocator fill entire slab") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate enough 32-byte objects to fill multiple slabs
    // Each slab is ~64 slots minimum, so 1000 should create multiple slabs
    constexpr u32 NUM_ALLOCS = 1000;

    struct AllocInfo {
        void* ptr;
        u64 gen;
        u32 magic;
    };

    std::vector<AllocInfo> allocs(NUM_ALLOCS);

    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        allocs[i].magic = 0xCAFE0000 | i;
        allocs[i].ptr = allocator.alloc(32, &allocs[i].gen);
        REQUIRE(allocs[i].ptr != nullptr);
        *static_cast<u32*>(allocs[i].ptr) = allocs[i].magic;
    }

    // Verify all
    for (u32 i = 0; i < NUM_ALLOCS; i++) {
        CHECK(*static_cast<u32*>(allocs[i].ptr) == allocs[i].magic);
    }

    // Free every other one
    for (u32 i = 0; i < NUM_ALLOCS; i += 2) {
        allocator.free(allocs[i].ptr);
        allocs[i].ptr = nullptr;
    }

    // Verify remaining
    for (u32 i = 1; i < NUM_ALLOCS; i += 2) {
        CHECK(*static_cast<u32*>(allocs[i].ptr) == allocs[i].magic);
    }

    // Free remaining
    for (u32 i = 1; i < NUM_ALLOCS; i += 2) {
        allocator.free(allocs[i].ptr);
    }

    allocator.shutdown();
}

TEST_CASE("Stress - ObjectHeader integrity under allocation pressure") {
    RoxyVM vm;
    REQUIRE(vm_init(&vm, VMConfig()));

    u32 type_id = register_object_type("IntegrityTest", 128, nullptr);

    constexpr u32 NUM_OBJECTS = 500;
    struct ObjRecord {
        void* data;
        u64 expected_gen;
        u32 expected_type_id;
    };

    std::vector<ObjRecord> records(NUM_OBJECTS);

    // Allocate all
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        records[i].data = object_alloc(&vm, type_id, 128);
        REQUIRE(records[i].data != nullptr);

        ObjectHeader* header = get_header_from_data(records[i].data);
        records[i].expected_gen = header->weak_generation;
        records[i].expected_type_id = header->type_id;

        // Verify header state
        CHECK(header->is_alive());
        CHECK(header->ref_count == 0);
        CHECK(header->type_id == type_id);
        CHECK(header->size == sizeof(ObjectHeader) + 128);
    }

    // Verify all headers still intact
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        ObjectHeader* header = get_header_from_data(records[i].data);
        CHECK(header->weak_generation == records[i].expected_gen);
        CHECK(header->type_id == records[i].expected_type_id);
        CHECK(header->is_alive());
    }

    // Free all
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        object_free(&vm, records[i].data);
    }

    vm_destroy(&vm);
}

// ============================================================================
// Slab Reclamation Tests
// ============================================================================

TEST_CASE("Unit - SlabAllocator reclaim_tombstoned basic") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate enough objects to COMPLETELY fill at least one slab
    // For 32-byte size class with 4KB pages: 4096/32 = 128 slots per slab
    // We allocate 150 to ensure at least one slab is completely filled
    constexpr u32 NUM_OBJECTS = 150;
    struct AllocInfo {
        void* ptr;
        u64 gen;
    };
    std::vector<AllocInfo> allocs(NUM_OBJECTS);

    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        allocs[i].ptr = allocator.alloc(32, &allocs[i].gen);
        REQUIRE(allocs[i].ptr != nullptr);
    }

    // Free all objects (they become tombstoned)
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        allocator.free(allocs[i].ptr);
    }

    // Reclaim tombstoned slabs - should reclaim at least the fully-filled first slab
    u32 pages_reclaimed = allocator.reclaim_tombstoned();
    CHECK(pages_reclaimed > 0);

    // Calling reclaim again should return 0 (already reclaimed)
    u32 pages_reclaimed_again = allocator.reclaim_tombstoned();
    CHECK(pages_reclaimed_again == 0);

    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator reclaim_tombstoned with live objects") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate enough to fill multiple slabs (128 slots per slab for 32-byte class)
    constexpr u32 NUM_OBJECTS = 300;
    struct AllocInfo {
        void* ptr;
        u64 gen;
    };
    std::vector<AllocInfo> allocs(NUM_OBJECTS);

    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        allocs[i].ptr = allocator.alloc(32, &allocs[i].gen);
        REQUIRE(allocs[i].ptr != nullptr);
    }

    // Free only the first 128 objects (first slab completely)
    // Keep some alive in subsequent slabs
    for (u32 i = 0; i < 128; i++) {
        allocator.free(allocs[i].ptr);
        allocs[i].ptr = nullptr;
    }

    // Reclaim should succeed for the first slab (all tombstoned)
    u32 pages_reclaimed = allocator.reclaim_tombstoned();
    CHECK(pages_reclaimed > 0);

    // Verify live objects are still accessible
    for (u32 i = 128; i < NUM_OBJECTS; i++) {
        CHECK(allocs[i].ptr != nullptr);
        // Write and read to verify memory is still valid
        *static_cast<u32*>(allocs[i].ptr) = 0xDEADBEEF;
        CHECK(*static_cast<u32*>(allocs[i].ptr) == 0xDEADBEEF);
    }

    // Free remaining objects
    for (u32 i = 128; i < NUM_OBJECTS; i++) {
        allocator.free(allocs[i].ptr);
    }

    // Now reclaim should reclaim more slabs
    u32 more_reclaimed = allocator.reclaim_tombstoned();
    CHECK(more_reclaimed > 0);

    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator reclaim_tombstoned idempotency") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate enough to fill at least one slab completely
    constexpr u32 NUM_OBJECTS = 150;
    std::vector<void*> ptrs(NUM_OBJECTS);
    u64 gen;

    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        ptrs[i] = allocator.alloc(32, &gen);
        REQUIRE(ptrs[i] != nullptr);
    }

    // Free all
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        allocator.free(ptrs[i]);
    }

    // Reclaim multiple times - should be safe
    u32 pages1 = allocator.reclaim_tombstoned();
    u32 pages2 = allocator.reclaim_tombstoned();
    u32 pages3 = allocator.reclaim_tombstoned();

    CHECK(pages1 > 0);
    CHECK(pages2 == 0);
    CHECK(pages3 == 0);

    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator reclaim_tombstoned memory still readable") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate exactly 128 objects to fill one slab completely
    // (128 slots per slab for 32-byte class with 4KB pages)
    constexpr u32 NUM_OBJECTS = 128;
    std::vector<void*> ptrs(NUM_OBJECTS);
    u64 gen;

    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        ptrs[i] = allocator.alloc(32, &gen);
        REQUIRE(ptrs[i] != nullptr);
        // Write non-zero data
        std::memset(ptrs[i], 0xAB, 32);
    }

    // Free all
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        allocator.free(ptrs[i]);
    }

    // Reclaim tombstoned slabs
    u32 pages_reclaimed = allocator.reclaim_tombstoned();
    CHECK(pages_reclaimed > 0);

    // After reclamation, memory should still be readable (for weak ref safety)
    // The actual content depends on platform - Windows MEM_RESET may or may not
    // zero memory immediately, but reading should not crash
    u64 sum = 0;
    for (u32 i = 0; i < NUM_OBJECTS; i++) {
        u8* bytes = static_cast<u8*>(ptrs[i]);
        for (u32 j = 0; j < 32; j++) {
            sum += bytes[j];  // Just read the memory to verify it's accessible
        }
    }
    // The sum will be something - we just want to verify no crash
    (void)sum;

    allocator.shutdown();
}

TEST_CASE("Unit - SlabAllocator reclaim_tombstoned multiple size classes") {
    SlabAllocator allocator;
    REQUIRE(allocator.init());

    // Allocate objects in multiple size classes
    // Each slab has 4KB / slot_size slots. We need to fill at least one slab per class.
    // 32-byte: 128 slots, 64-byte: 64 slots, 128-byte: 32 slots, 256-byte: 16 slots
    constexpr u32 SIZE_CLASSES[] = {32, 64, 128, 256};
    constexpr u32 NUM_CLASSES = sizeof(SIZE_CLASSES) / sizeof(SIZE_CLASSES[0]);
    constexpr u32 OBJECTS_PER_CLASS = 150;  // Enough to fill at least one slab for all classes

    std::vector<std::vector<void*>> allocs(NUM_CLASSES);
    u64 gen;

    for (u32 c = 0; c < NUM_CLASSES; c++) {
        allocs[c].resize(OBJECTS_PER_CLASS);
        for (u32 i = 0; i < OBJECTS_PER_CLASS; i++) {
            allocs[c][i] = allocator.alloc(SIZE_CLASSES[c], &gen);
            REQUIRE(allocs[c][i] != nullptr);
        }
    }

    // Free all objects in all size classes
    for (u32 c = 0; c < NUM_CLASSES; c++) {
        for (u32 i = 0; i < OBJECTS_PER_CLASS; i++) {
            allocator.free(allocs[c][i]);
        }
    }

    // Reclaim should reclaim slabs from all size classes
    u32 pages_reclaimed = allocator.reclaim_tombstoned();
    CHECK(pages_reclaimed > 0);

    // Second call should return 0
    u32 pages_again = allocator.reclaim_tombstoned();
    CHECK(pages_again == 0);

    allocator.shutdown();
}

TEST_CASE("Stress - Concurrent-like access pattern simulation") {
    // Simulate what might happen with multiple "threads" (sequentially)
    // Each "thread" has its own set of objects it manages
    RoxyVM vm;
    REQUIRE(vm_init(&vm, VMConfig()));

    u32 type_id = register_object_type("ThreadSimObject", 32, nullptr);

    constexpr u32 NUM_THREADS = 4;
    constexpr u32 OBJECTS_PER_THREAD = 100;
    constexpr u32 ITERATIONS = 50;

    struct ThreadState {
        std::vector<void*> objects;
        std::vector<u64> generations;
    };

    std::array<ThreadState, NUM_THREADS> threads;
    std::mt19937 rng(99999);

    // Initial allocation for each "thread"
    for (u32 t = 0; t < NUM_THREADS; t++) {
        threads[t].objects.resize(OBJECTS_PER_THREAD);
        threads[t].generations.resize(OBJECTS_PER_THREAD);
        for (u32 i = 0; i < OBJECTS_PER_THREAD; i++) {
            threads[t].objects[i] = object_alloc(&vm, type_id, 32);
            REQUIRE(threads[t].objects[i] != nullptr);
            threads[t].generations[i] = weak_ref_create(threads[t].objects[i]);
        }
    }

    // Simulate interleaved operations
    for (u32 iter = 0; iter < ITERATIONS; iter++) {
        // Each thread does some work
        for (u32 t = 0; t < NUM_THREADS; t++) {
            // Pick a random object in this thread
            u32 idx = rng() % OBJECTS_PER_THREAD;

            if (threads[t].objects[idx] != nullptr) {
                // Verify weak ref still valid
                CHECK(weak_ref_valid(threads[t].objects[idx], threads[t].generations[idx]));

                // 30% chance to free and reallocate
                if (rng() % 100 < 30) {
                    object_free(&vm, threads[t].objects[idx]);

                    // Verify weak ref now invalid
                    CHECK(!weak_ref_valid(threads[t].objects[idx], threads[t].generations[idx]));

                    // Reallocate
                    threads[t].objects[idx] = object_alloc(&vm, type_id, 32);
                    REQUIRE(threads[t].objects[idx] != nullptr);
                    threads[t].generations[idx] = weak_ref_create(threads[t].objects[idx]);
                }
            }
        }

        // Cross-thread verification: check other threads' objects are still valid
        for (u32 t = 0; t < NUM_THREADS; t++) {
            for (u32 i = 0; i < OBJECTS_PER_THREAD; i++) {
                if (threads[t].objects[i] != nullptr) {
                    CHECK(weak_ref_valid(threads[t].objects[i], threads[t].generations[i]));
                }
            }
        }
    }

    // Cleanup
    for (u32 t = 0; t < NUM_THREADS; t++) {
        for (u32 i = 0; i < OBJECTS_PER_THREAD; i++) {
            if (threads[t].objects[i] != nullptr) {
                object_free(&vm, threads[t].objects[i]);
            }
        }
    }

    vm_destroy(&vm);
}

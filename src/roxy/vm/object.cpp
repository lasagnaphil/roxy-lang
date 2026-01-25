#include "roxy/vm/object.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/slab_allocator.hpp"
#include "roxy/core/vector.hpp"

#include <cstdlib>
#include <cassert>
#include <cstring>

namespace rx {

// Global type registry
static Vector<ObjectTypeInfo> g_type_registry;

u32 register_object_type(const char* name, u32 size, void (*destructor)(RoxyVM*, void*)) {
    u32 type_id = g_type_registry.size();
    ObjectTypeInfo info;
    info.type_id = type_id;
    info.size = size;
    info.name = name;
    info.destructor = destructor;
    g_type_registry.push_back(info);
    return type_id;
}

const ObjectTypeInfo* get_object_type(u32 type_id) {
    if (type_id >= g_type_registry.size()) {
        return nullptr;
    }
    return &g_type_registry[type_id];
}

void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size) {
    if (vm == nullptr || vm->allocator == nullptr) {
        // Fallback to malloc if no allocator available
        u32 total_size = sizeof(ObjectHeader) + data_size;
        void* mem = std::malloc(total_size);
        if (mem == nullptr) {
            return nullptr;
        }

        ObjectHeader* header = static_cast<ObjectHeader*>(mem);
        header->weak_generation = 0;
        header->ref_count = 0;
        header->type_id = type_id;
        header->size = total_size;
        header->flags = ObjectHeader::FLAG_ALIVE;

        void* data = header->data();
        std::memset(data, 0, data_size);
        return data;
    }

    u32 total_size = sizeof(ObjectHeader) + data_size;
    u64 generation;
    void* mem = vm->allocator->alloc(total_size, &generation);
    if (mem == nullptr) {
        return nullptr;
    }

    // Initialize header
    // ref_count starts at 0 for constraint reference model:
    // uniq doesn't affect ref_count, only ref borrows increment it
    ObjectHeader* header = static_cast<ObjectHeader*>(mem);
    header->weak_generation = generation;
    header->ref_count = 0;
    header->type_id = type_id;
    header->size = total_size;
    header->flags = ObjectHeader::FLAG_ALIVE;

    // Object data is already zeroed by the allocator

    return header->data();
}

bool ref_dec(RoxyVM* vm, void* data) {
    (void)vm;  // Not used in constraint reference model
    if (data == nullptr) return false;

    ObjectHeader* header = get_header_from_data(data);
    assert(header->ref_count > 0);

    // In constraint reference model, ref_count tracks borrows, not ownership.
    // Decrement borrow count - deallocation is handled by owner via delete.
    header->ref_count--;
    return false;  // Never deallocates - owner is responsible
}

u64 weak_ref_create(void* data) {
    if (data == nullptr) return 0;
    ObjectHeader* header = get_header_from_data(data);
    return header->weak_generation;
}

bool weak_ref_valid(void* data, u64 generation) {
    if (data == nullptr) return false;

    // Safe to read: memory is always mapped (active or tombstoned)
    // Tombstoned memory returns zeros, so is_alive() will be false
    ObjectHeader* header = get_header_from_data(data);
    return header->is_alive() && (header->weak_generation == generation);
}

void weak_ref_invalidate(void* data, u64 new_generation) {
    if (data == nullptr) return;
    ObjectHeader* header = get_header_from_data(data);
    header->weak_generation = new_generation;
    header->flags = ObjectHeader::FLAG_TOMBSTONE;
}

void object_free(RoxyVM* vm, void* data) {
    if (data == nullptr) return;

    ObjectHeader* header = get_header_from_data(data);

    // Call destructor if registered
    const ObjectTypeInfo* type_info = get_object_type(header->type_id);
    if (type_info && type_info->destructor) {
        type_info->destructor(vm, data);
    }

    if (vm == nullptr || vm->allocator == nullptr) {
        // Fallback: direct free if no allocator
        header->flags = ObjectHeader::FLAG_TOMBSTONE;
        std::free(header);
        return;
    }

    // Mark as tombstone - weak_ref_valid() checks is_alive() which returns false
    // when FLAG_ALIVE is not set. FLAG_TOMBSTONE (0x02) doesn't include FLAG_ALIVE (0x01).
    header->flags = ObjectHeader::FLAG_TOMBSTONE;

    // Zero the object data (not the header - keep flags/generation intact)
    u32 data_size = header->size - sizeof(ObjectHeader);
    std::memset(data, 0, data_size);

    // Free via slab allocator - updates bookkeeping, memory stays mapped
    // Note: Tombstoned slots are never reused, so generation doesn't need to change.
    // If slot recycling is added later, allocator should set new random generation.
    vm->allocator->free(header);
}

}

#include "roxy/vm/object.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/slab_allocator.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/core/vector.hpp"

#include <cstdlib>
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

void init_type_registry() {
    static bool initialized = false;
    if (initialized) return;
    register_list_type();
    register_string_type();
    initialized = true;
}

void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size) {
    if (vm == nullptr || vm->allocator == nullptr) {
        // Fallback to malloc if no allocator available
        static u64 s_malloc_generation = 1;

        u32 total_size = sizeof(ObjectHeader) + data_size;
        void* mem = std::malloc(total_size);
        if (mem == nullptr) {
            return nullptr;
        }

        ObjectHeader* header = static_cast<ObjectHeader*>(mem);
        header->weak_generation = s_malloc_generation++;
        if (s_malloc_generation == 0) s_malloc_generation = 1;  // skip zero
        header->ref_count = 0;
        header->type_id = type_id;

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

    // Object data is already zeroed by the allocator

    return header->data();
}

bool ref_dec(RoxyVM* vm, void* data) {
    if (data == nullptr) return false;

    ObjectHeader* header = get_header_from_data(data);
    if (header->ref_count == 0) {
        if (vm) vm->error = "ref_dec: reference count already zero";
        return false;  // error
    }

    // In constraint reference model, ref_count tracks borrows, not ownership.
    // Decrement borrow count - deallocation is handled by owner via delete.
    header->ref_count--;
    return true;  // success
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
        header->weak_generation = 0;
        std::free(header);
        return;
    }

    // Mark as dead — weak_ref_valid() checks is_alive() which returns false
    // when weak_generation == 0.
    header->weak_generation = 0;

    // Free via slab allocator — zeroes the entire slot (header + data),
    // then updates bookkeeping. Memory stays mapped for safe weak ref reads.
    vm->allocator->free(header);
}

}

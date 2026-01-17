#include "roxy/vm/object.hpp"
#include "roxy/core/vector.hpp"

#include <cstdlib>
#include <cassert>

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
    (void)vm;  // May be used for VM-specific allocators in the future

    u32 total_size = sizeof(ObjectHeader) + data_size;
    void* mem = std::malloc(total_size);
    if (mem == nullptr) {
        return nullptr;
    }

    // Initialize header
    ObjectHeader* header = static_cast<ObjectHeader*>(mem);
    header->ref_count = 1;
    header->weak_generation = 0;
    header->type_id = type_id;
    header->size = total_size;

    // Zero-initialize object data
    void* data = header->data();
    std::memset(data, 0, data_size);

    return data;
}

bool ref_dec(RoxyVM* vm, void* data) {
    if (data == nullptr) return false;

    ObjectHeader* header = get_header_from_data(data);
    assert(header->ref_count > 0);

    header->ref_count--;
    if (header->ref_count == 0) {
        // Call destructor if registered
        const ObjectTypeInfo* type_info = get_object_type(header->type_id);
        if (type_info && type_info->destructor) {
            type_info->destructor(vm, data);
        }

        // Invalidate weak references before freeing
        weak_ref_invalidate(data);

        // Free memory
        std::free(header);
        return true;
    }
    return false;
}

u32 weak_ref_create(void* data) {
    if (data == nullptr) return 0;
    ObjectHeader* header = get_header_from_data(data);
    return header->weak_generation;
}

bool weak_ref_valid(void* data, u32 generation) {
    if (data == nullptr) return false;
    ObjectHeader* header = get_header_from_data(data);
    return header->weak_generation == generation;
}

void weak_ref_invalidate(void* data) {
    if (data == nullptr) return;
    ObjectHeader* header = get_header_from_data(data);
    header->weak_generation++;
}

void object_free(RoxyVM* vm, void* data) {
    if (data == nullptr) return;

    ObjectHeader* header = get_header_from_data(data);

    // Call destructor if registered
    const ObjectTypeInfo* type_info = get_object_type(header->type_id);
    if (type_info && type_info->destructor) {
        type_info->destructor(vm, data);
    }

    // Invalidate weak references
    weak_ref_invalidate(data);

    // Free memory
    std::free(header);
}

}

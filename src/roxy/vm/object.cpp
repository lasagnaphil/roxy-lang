#include "roxy/vm/object.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/rt/slab_allocator.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/map.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/core/vector.hpp"

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

void init_type_registry() {
    static bool initialized = false;
    if (initialized) return;
    register_list_type();
    register_string_type();
    register_map_type();
    initialized = true;
}

void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size) {
    assert(vm != nullptr && vm->allocator != nullptr);

    // Route through the per-VM `roxy_allocator` vtable — same slab as the
    // direct call but goes through the same allocation path AOT-compiled
    // programs use, so any future allocator-side hook (statistics,
    // tracing) covers both modes uniformly.
    u32 total_size = sizeof(ObjectHeader) + data_size;
    u64 generation = 0;
    void* mem = vm->slab_vtable.alloc(vm->slab_vtable.userdata,
                                      total_size, &generation);
    if (!mem) return nullptr;

    // Initialize header. ref_count starts at 0 for the constraint
    // reference model: uniq doesn't affect ref_count, only ref borrows do.
    auto* header = static_cast<ObjectHeader*>(mem);
    header->weak_generation = generation;
    header->ref_count = 0;
    header->type_id = type_id;
    return header_data(header);
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
    return is_alive(header) && (header->weak_generation == generation);
}

void object_free(RoxyVM* vm, void* data) {
    if (data == nullptr) return;
    assert(vm != nullptr && vm->allocator != nullptr);

    ObjectHeader* header = get_header_from_data(data);

    // Call destructor if registered. The destructor still receives `vm`
    // because some impls (e.g. the map-dispatch unregister hook) need
    // access to per-VM state.
    const ObjectTypeInfo* type_info = get_object_type(header->type_id);
    if (type_info && type_info->destructor) {
        type_info->destructor(vm, data);
    }

    // Free via the per-VM `roxy_allocator` vtable — the slab impl tombstones
    // `weak_generation` (zeros the slot) and stays-mapped so weak refs see
    // "dead". Same allocator path used by `roxy_free`.
    vm->slab_vtable.free(vm->slab_vtable.userdata, header);
}

}

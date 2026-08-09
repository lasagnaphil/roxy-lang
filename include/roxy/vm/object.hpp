#pragma once

#include "roxy/core/string_view.hpp"
#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"

namespace rx {

// `ObjectHeader` is the unified runtime header used by both the VM and AOT-
// compiled programs. It's a typedef for the C struct in `roxy_rt.h` (16 bytes:
// u64 weak_generation, u32 ref_count, u32 type_id) so memory layout matches
// across both runtimes.
using ObjectHeader = roxy_object_header;

// Check if the object is alive (not tombstoned).
// `weak_generation == 0` means dead; non-zero means alive.
inline bool is_alive(const ObjectHeader* hdr) { return hdr && hdr->weak_generation != 0; }

// Get pointer to object data (immediately after the header).
inline void* header_data(ObjectHeader* hdr) {
    return reinterpret_cast<u8*>(hdr) + sizeof(ObjectHeader);
}

inline const void* header_data(const ObjectHeader* hdr) {
    return reinterpret_cast<const u8*>(hdr) + sizeof(ObjectHeader);
}

// Get object header from a data pointer.
inline ObjectHeader* get_header_from_data(void* data) {
    return reinterpret_cast<ObjectHeader*>(reinterpret_cast<u8*>(data) - sizeof(ObjectHeader));
}

inline const ObjectHeader* get_header_from_data(const void* data) {
    return reinterpret_cast<const ObjectHeader*>(reinterpret_cast<const u8*>(data) -
                                                 sizeof(ObjectHeader));
}

// Forward declaration
struct RoxyVM;

// Allocate a new object with the given type and size
// Returns pointer to object data (not header)
void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size);

// Increment reference count
inline void ref_inc(void* data) {
    if (data == nullptr)
        return;
    ObjectHeader* header = get_header_from_data(data);
    header->ref_count++;
}

// Decrement reference count. Returns false on error (count was already zero).
bool ref_dec(RoxyVM* vm, void* data);

// Create a weak reference from a strong pointer
// Returns the current 64-bit generation for validation
u64 weak_ref_create(void* data);

// Check if a weak reference is still valid
// Uses 64-bit generation; weak_generation == 0 means tombstoned
bool weak_ref_valid(void* data, u64 generation);

// Deallocate an object (for explicit delete)
void object_free(RoxyVM* vm, void* data);

// Object type registry
struct ObjectTypeInfo {
    u32 type_id;
    u32 size; // Size of object data (not including header)
    // Type name for debugging. A StringView, not a `const char*`: the names that
    // matter — user struct names, from `BCTypeInfo` — point into the module's
    // source text and are NOT null-terminated, so printing one as `%s` ran off
    // the end of the name and into the rest of the file. Print with `%.*s`.
    StringView name;
    void (*destructor)(RoxyVM* vm, void* data); // Optional destructor
};

// NOTE: The type registry is global and NOT thread-safe.
// All VM instances must be used from the same thread, or external
// synchronization must be provided by the host application.

// Initialize built-in types (list, string). Called automatically by vm_init().
void init_type_registry();

// Register a new object type
// `name` is borrowed, not copied — it must outlive the registry (a string
// literal, or storage owned by the module being loaded).
u32 register_object_type(StringView name, u32 size, void (*destructor)(RoxyVM*, void*) = nullptr);

// Get type info by ID
const ObjectTypeInfo* get_object_type(u32 type_id);

} // namespace rx

#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Object header - precedes all heap-allocated objects
// Memory layout: [ObjectHeader][object data...]
// Size: 16 bytes
struct ObjectHeader {
    u64 weak_generation;    // 64-bit random generation for weak refs; 0 = dead/tombstoned
    u32 ref_count;          // Reference count for ref borrows
    u32 type_id;            // Type identifier for runtime type info

    // ref_count starts at 0 for constraint reference model:
    // uniq doesn't affect ref_count, only ref borrows increment it
    ObjectHeader()
        : weak_generation(0)
        , ref_count(0)
        , type_id(0)
    {}

    // Check if the object is alive (not tombstoned)
    // weak_generation == 0 means dead; non-zero means alive
    bool is_alive() const { return weak_generation != 0; }

    // Get pointer to object data (after header)
    void* data() {
        return reinterpret_cast<u8*>(this) + sizeof(ObjectHeader);
    }

    const void* data() const {
        return reinterpret_cast<const u8*>(this) + sizeof(ObjectHeader);
    }
};

// Get object header from a data pointer
inline ObjectHeader* get_header_from_data(void* data) {
    return reinterpret_cast<ObjectHeader*>(
        reinterpret_cast<u8*>(data) - sizeof(ObjectHeader));
}

inline const ObjectHeader* get_header_from_data(const void* data) {
    return reinterpret_cast<const ObjectHeader*>(
        reinterpret_cast<const u8*>(data) - sizeof(ObjectHeader));
}

// Forward declaration
struct RoxyVM;

// Allocate a new object with the given type and size
// Returns pointer to object data (not header)
void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size);

// Increment reference count
inline void ref_inc(void* data) {
    if (data == nullptr) return;
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
    u32 size;                   // Size of object data (not including header)
    const char* name;           // Type name for debugging
    void (*destructor)(RoxyVM* vm, void* data);  // Optional destructor
};

// NOTE: The type registry is global and NOT thread-safe.
// All VM instances must be used from the same thread, or external
// synchronization must be provided by the host application.

// Initialize built-in types (list, string). Called automatically by vm_init().
void init_type_registry();

// Register a new object type
u32 register_object_type(const char* name, u32 size, void (*destructor)(RoxyVM*, void*) = nullptr);

// Get type info by ID
const ObjectTypeInfo* get_object_type(u32 type_id);

}

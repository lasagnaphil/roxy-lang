#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Object header - precedes all heap-allocated objects
// Memory layout: [ObjectHeader][object data...]
struct ObjectHeader {
    u32 ref_count;          // Reference count for uniq/ref
    u32 weak_generation;    // Generation counter for weak reference validation
    u32 type_id;            // Type identifier for runtime type info
    u32 size;               // Total size of object including header

    ObjectHeader() : ref_count(1), weak_generation(0), type_id(0), size(0) {}

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

// Decrement reference count, deallocate if zero
// Returns true if object was deallocated
bool ref_dec(RoxyVM* vm, void* data);

// Create a weak reference from a strong pointer
// Returns the current generation for validation
u32 weak_ref_create(void* data);

// Check if a weak reference is still valid
bool weak_ref_valid(void* data, u32 generation);

// Invalidate all weak references to an object (bump generation)
void weak_ref_invalidate(void* data);

// Deallocate an object (for explicit delete)
void object_free(RoxyVM* vm, void* data);

// Object type registry
struct ObjectTypeInfo {
    u32 type_id;
    u32 size;                   // Size of object data (not including header)
    const char* name;           // Type name for debugging
    void (*destructor)(RoxyVM* vm, void* data);  // Optional destructor
};

// Register a new object type
u32 register_object_type(const char* name, u32 size, void (*destructor)(RoxyVM*, void*) = nullptr);

// Get type info by ID
const ObjectTypeInfo* get_object_type(u32 type_id);

}

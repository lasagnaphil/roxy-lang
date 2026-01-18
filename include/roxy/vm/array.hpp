#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/value.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;

// Array header - stored in object data after ObjectHeader
// Memory layout: [ObjectHeader][ArrayHeader][Value * length]
struct ArrayHeader {
    u32 length;
    u32 capacity;
};

// Get the ArrayHeader from array data pointer (data is right after ObjectHeader)
inline ArrayHeader* get_array_header(void* data) {
    return static_cast<ArrayHeader*>(data);
}

inline const ArrayHeader* get_array_header(const void* data) {
    return static_cast<const ArrayHeader*>(data);
}

// Get pointer to array elements (follows ArrayHeader)
inline Value* array_elements(void* data) {
    return reinterpret_cast<Value*>(
        static_cast<u8*>(data) + sizeof(ArrayHeader));
}

inline const Value* array_elements(const void* data) {
    return reinterpret_cast<const Value*>(
        static_cast<const u8*>(data) + sizeof(ArrayHeader));
}

// Allocate a new array with given length
// Returns pointer to array data (ArrayHeader followed by elements)
void* array_alloc(RoxyVM* vm, u32 length);

// Get array length
inline u32 array_length(const void* data) {
    return get_array_header(data)->length;
}

// Get element at index (bounds-checked)
// Returns true on success, false on error (sets error message)
bool array_get(void* data, i64 index, Value& out, const char** error);

// Set element at index (bounds-checked)
// Returns true on success, false on error (sets error message)
bool array_set(void* data, i64 index, Value value, const char** error);

// Register the array object type (call at initialization)
u32 register_array_type();

// Get the registered array type ID
u32 get_array_type_id();

}

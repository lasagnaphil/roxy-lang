#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/value.hpp"

namespace rx {

// Forward declarations
struct RoxyVM;

// List header - stored in object data after ObjectHeader
// Memory layout: [ObjectHeader][ListHeader]
// Elements are in a separate malloc'd buffer (allows realloc on push without moving the header)
struct ListHeader {
    u32 length;
    u32 capacity;
    Value* elements;   // separate malloc'd buffer (nullptr if capacity == 0)
};

// Get the ListHeader from list data pointer (data is right after ObjectHeader)
inline ListHeader* get_list_header(void* data) {
    return static_cast<ListHeader*>(data);
}

inline const ListHeader* get_list_header(const void* data) {
    return static_cast<const ListHeader*>(data);
}

// Allocate a new list with given capacity (length starts at 0)
// Returns pointer to list data (ListHeader)
void* list_alloc(RoxyVM* vm, u32 capacity);

// Get list length
inline u32 list_length(const void* data) {
    return get_list_header(data)->length;
}

// Get list capacity
inline u32 list_capacity(const void* data) {
    return get_list_header(data)->capacity;
}

// Get element at index (bounds-checked)
// Returns true on success, false on error (sets error message)
bool list_get(void* data, i64 index, Value& out, const char** error);

// Set element at index (bounds-checked)
// Returns true on success, false on error (sets error message)
bool list_set(void* data, i64 index, Value value, const char** error);

// Push an element to the end of the list (grows if needed)
void list_push(void* data, Value value);

// Pop an element from the end of the list
// Returns the popped element
Value list_pop(void* data);

// Deep-copy a list (allocates a new list with same elements)
void* list_copy(RoxyVM* vm, void* src);

// Register the list object type (call at initialization)
u32 register_list_type();

// Get the registered list type ID
u32 get_list_type_id();

}

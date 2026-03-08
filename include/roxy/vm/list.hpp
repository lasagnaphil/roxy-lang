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
    u32 element_slot_count;  // number of u32 slots per element (default 2)
    bool element_is_inline;  // true: register holds value directly; false: register holds pointer to data
    u32* elements;           // slot-based buffer: element[i] at &elements[i * element_slot_count]
};

// Get the ListHeader from list data pointer (data is right after ObjectHeader)
inline ListHeader* get_list_header(void* data) {
    return static_cast<ListHeader*>(data);
}

inline const ListHeader* get_list_header(const void* data) {
    return static_cast<const ListHeader*>(data);
}

// Get pointer to element slot data at given index
inline u32* list_element_ptr(ListHeader* header, u32 index) {
    return header->elements + index * header->element_slot_count;
}

inline const u32* list_element_ptr(const ListHeader* header, u32 index) {
    return header->elements + index * header->element_slot_count;
}

// Allocate a new list with given capacity and element slot count (length starts at 0)
// element_is_inline: true for primitives (value in register), false for structs (pointer in register)
// Returns pointer to list data (ListHeader)
void* list_alloc(RoxyVM* vm, u32 capacity, u32 element_slot_count = 2, bool element_is_inline = true);

// Get list length
inline u32 list_length(const void* data) {
    return get_list_header(data)->length;
}

// Get list capacity
inline u32 list_capacity(const void* data) {
    return get_list_header(data)->capacity;
}

// ── Slot-based API (used by interpreter and native functions) ──

// Get pointer to element at index (bounds-checked)
// Returns pointer to element slot data, or nullptr on error (sets error message)
u32* list_get_ptr(void* data, i64 index, const char** error);

// Set element at index by copying slots from src (bounds-checked)
bool list_set_slots(void* data, i64 index, const u32* src, const char** error);

// Push element by copying element_slot_count slots from src
void list_push_slots(void* data, const u32* src);

// Pop element, returns pointer to the popped element's slot data
// The pointer is valid until the next mutation.
u32* list_pop_ptr(void* data);

// ── Value-based wrappers (backward compatibility for RoxyList<T>) ──

// Get element at index (bounds-checked, 2-slot elements only)
bool list_get(void* data, i64 index, Value& out, const char** error);

// Set element at index (bounds-checked, 2-slot elements only)
bool list_set(void* data, i64 index, Value value, const char** error);

// Push a Value element (2-slot elements only)
void list_push(void* data, Value value);

// Pop a Value element (2-slot elements only)
Value list_pop(void* data);

// Deep-copy a list (allocates a new list with same elements)
void* list_copy(RoxyVM* vm, void* src);

// Register the list object type (call at initialization)
u32 register_list_type();

// Get the registered list type ID
u32 get_list_type_id();

}

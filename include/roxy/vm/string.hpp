#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Forward declaration
struct RoxyVM;

// String header - stored in object data after ObjectHeader
// Memory layout: [ObjectHeader][StringHeader][char data + null terminator]
struct StringHeader {
    u32 length;    // String length (excluding null terminator)
    u32 capacity;  // Allocated capacity (including null terminator)
};

// Get the StringHeader from string data pointer (data points to StringHeader)
inline StringHeader* get_string_header(void* data) {
    return static_cast<StringHeader*>(data);
}

inline const StringHeader* get_string_header(const void* data) {
    return static_cast<const StringHeader*>(data);
}

// Get pointer to character data (follows StringHeader)
inline char* string_chars(void* data) {
    return reinterpret_cast<char*>(
        static_cast<u8*>(data) + sizeof(StringHeader));
}

inline const char* string_chars(const void* data) {
    return reinterpret_cast<const char*>(
        static_cast<const u8*>(data) + sizeof(StringHeader));
}

// Get string length
inline u32 string_length(const void* data) {
    return get_string_header(data)->length;
}

// Allocate a new string (copies data)
// Returns pointer to string data (StringHeader followed by chars)
void* string_alloc(RoxyVM* vm, const char* data, u32 length);

// Concatenate two strings, returns new StringObject
void* string_concat(RoxyVM* vm, void* str1, void* str2);

// Compare two strings for equality
bool string_equals(const void* str1, const void* str2);

// Register the string object type (call at initialization)
u32 register_string_type();

// Get the registered string type ID
u32 get_string_type_id();

}

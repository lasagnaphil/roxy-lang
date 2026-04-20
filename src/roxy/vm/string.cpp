#include "roxy/vm/string.hpp"
#include "roxy/vm/object.hpp"

#include <cstring>

#define XXH_INLINE_ALL
#include "roxy/core/xxhash.h"

namespace rx {

// Global string type ID (registered once at startup)
static u32 g_string_type_id = UINT32_MAX;

u32 register_string_type() {
    if (g_string_type_id == UINT32_MAX) {
        g_string_type_id = register_object_type("string", 0, nullptr);
    }
    return g_string_type_id;
}

u32 get_string_type_id() {
    return g_string_type_id;
}

void* string_alloc(RoxyVM* vm, const char* data, u32 length) {
    // Calculate total data size: StringHeader + (chars + null terminator)
    u32 data_size = sizeof(StringHeader) + length + 1;

    // Allocate using object system
    void* string_data = object_alloc(vm, g_string_type_id, data_size);
    if (!string_data) {
        return nullptr;
    }

    // Initialize string header
    StringHeader* header = get_string_header(string_data);
    header->length = length;

    // Copy string data and null-terminate
    char* chars = string_chars(string_data);
    if (data && length > 0) {
        memcpy(chars, data, length);
    }
    chars[length] = '\0';

    // Cache a 32-bit hash (low bits of XXH3_64bits) once at allocation so Map
    // lookups don't walk the bytes on every op. The MapHeader's capacity is a
    // u32 and the probe mask is computed as (capacity - 1), so the upper 32
    // bits of a 64-bit hash would be discarded anyway.
    header->hash = static_cast<u32>(XXH3_64bits(chars, length));

    return string_data;
}

void* string_concat(RoxyVM* vm, void* str1, void* str2) {
    if (!str1 || !str2) {
        return nullptr;
    }

    u32 len1 = string_length(str1);
    u32 len2 = string_length(str2);
    u32 total_len = len1 + len2;

    // Allocate new string with combined length
    void* result = string_alloc(vm, nullptr, total_len);
    if (!result) {
        return nullptr;
    }

    // Copy both strings into the result
    char* result_chars = string_chars(result);
    memcpy(result_chars, string_chars(str1), len1);
    memcpy(result_chars + len1, string_chars(str2), len2);
    result_chars[total_len] = '\0';

    // Re-hash now that the real bytes are in place (string_alloc hashed the
    // zeroed buffer we asked for).
    get_string_header(result)->hash =
        static_cast<u32>(XXH3_64bits(result_chars, total_len));

    return result;
}

bool string_equals(const void* str1, const void* str2) {
    if (str1 == str2) {
        return true;  // Same pointer
    }
    if (!str1 || !str2) {
        return false;  // One is null
    }

    u32 len1 = string_length(str1);
    u32 len2 = string_length(str2);

    if (len1 != len2) {
        return false;  // Different lengths
    }

    // Compare character data
    return memcmp(string_chars(str1), string_chars(str2), len1) == 0;
}

}

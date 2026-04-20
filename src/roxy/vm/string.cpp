#include "roxy/vm/string.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/string_intern.hpp"
#include "roxy/vm/vm.hpp"

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
    // Content-keyed interning. Probing before allocating dedups the hot cases:
    // - LOAD_CONST of the same string literal across many call sites
    // - f-string numeric conversions that repeat the same digit ("0", "1", ...)
    // - str_substr / str_from_code slices that recur
    // Strings are immutable copyable values in Roxy (TypeKind::String is not
    // in Type::noncopyable()), so sharing a pointer across variables is safe.
    // Skip the probe only for the length=0/data=null case that string_concat
    // uses as a "give me a buffer" call — that path rewrites the bytes before
    // anyone else sees the object.
    if (vm->string_intern && data && length > 0) {
        StringView key(data, length);
        auto it = vm->string_intern->table.find(key);
        if (it != vm->string_intern->table.end()) {
            return it->second;
        }
    }

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

    // Register the new string in the intern table. Key is a StringView over
    // the object's own chars — stable for the object's (= VM's) lifetime.
    if (vm->string_intern && data && length > 0) {
        vm->string_intern->table[StringView(chars, length)] = string_data;
    }

    return string_data;
}

void* string_concat(RoxyVM* vm, void* str1, void* str2) {
    if (!str1 || !str2) {
        return nullptr;
    }

    u32 len1 = string_length(str1);
    u32 len2 = string_length(str2);
    u32 total_len = len1 + len2;

    // Build the concatenated content in a temp buffer so we can intern on real
    // bytes. Short strings use the stack; longer ones take a one-shot malloc.
    char stack_buf[256];
    char* buf = (total_len < sizeof(stack_buf))
        ? stack_buf
        : static_cast<char*>(malloc(total_len + 1));
    if (!buf) {
        return nullptr;
    }

    memcpy(buf, string_chars(str1), len1);
    memcpy(buf + len1, string_chars(str2), len2);
    // string_alloc doesn't read past `length`, but null-terminating costs
    // nothing and keeps the buffer sane for any debug prints.
    buf[total_len] = '\0';

    void* result = string_alloc(vm, buf, total_len);

    if (buf != stack_buf) {
        free(buf);
    }
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

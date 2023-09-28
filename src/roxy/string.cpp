#include "roxy/string.hpp"
#include "roxy/core/xxhash.h"

#include <cstddef>
#include <cstdlib>
#include <new>

namespace rx {

ObjString *allocate_obj_string(u32 length) {
    void* raw_data = malloc(offsetof(ObjString, chars[length + 1]));
    new (raw_data) ObjString();
    auto str = static_cast<ObjString*>(raw_data);
    str->length = length;
    return str;
}

ObjString *create_obj_string(const char *chars, u32 length) {
    auto str = allocate_obj_string(length);
    memcpy(str->chars, chars, length);
    str->hash = XXH3_64bits(chars, length);
    str->chars[length] = 0;
    return str;
}

void free_obj_string(ObjString* obj_string) {
    obj_string->~ObjString();
    free(obj_string);
}

ObjString* create_obj_string_with_known_hash(const char* chars, u32 length, u64 hash) {
    auto str = allocate_obj_string(length);
    memcpy(str->chars, chars, length);
    str->hash = hash;
    str->chars[length] = 0;
    return str;
}

ObjString* concat_string(ObjString* a, ObjString* b) {
    int32_t length = a->length + b->length;
    auto result = allocate_obj_string(length);
    memcpy(result->chars, a->chars, a->length);
    memcpy(result->chars + a->length, b->chars, b->length);
    result->hash = XXH3_64bits(result->chars, length);
    result->chars[length] = 0;
    return result;
}

}
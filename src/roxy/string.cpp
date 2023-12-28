#include "roxy/string.hpp"
#include "roxy/core/xxhash.h"

#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

namespace rx {

ObjString* ObjString::allocate(u32 length) {
    void* raw_data = malloc(offsetof(ObjString, m_chars[length + 1]));
    new (raw_data) ObjString();
    auto str = static_cast<ObjString*>(raw_data);
    str->m_length = length;
    return str;
}

ObjString* ObjString::create(const char* chars, u32 length) {
    auto str = ObjString::allocate(length);
    memcpy(str->m_chars, chars, length);
    str->m_hash = XXH3_64bits(chars, length);
    str->m_chars[length] = 0;
    return str;
}

ObjString* ObjString::create_with_known_hash(const char* chars, u32 length, u64 hash) {
    auto str = ObjString::allocate(length);
    memcpy(str->m_chars, chars, length);
    str->m_hash = hash;
    str->m_chars[length] = 0;
    return str;
}

ObjString* ObjString::from_bool(bool value) {
    static u64 true_hash = 0;
    static u64 false_hash = 0;
    if (value) {
        if (true_hash == 0) true_hash = XXH3_64bits("true", 4);
        return create_with_known_hash("true", 4, true_hash);
    }
    else {
        if (false_hash == 0) false_hash = XXH3_64bits("false", 5);
        return create_with_known_hash("false", 5, false_hash);
    }
}

// TODO: find allocation-free way to convert these to strings
ObjString* ObjString::from_i32(i32 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::from_i64(i64 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::from_u32(u32 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::from_u64(u64 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::from_f32(f32 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::from_f64(f64 value) {
    auto str = std::to_string(value);
    return ObjString::create(str.data(), (u32)str.length());
}

ObjString* ObjString::concat(ObjString* a, ObjString* b) {
    int32_t length = a->m_length + b->m_length;
    auto result = ObjString::allocate(length);
    memcpy(result->m_chars, a->m_chars, a->m_length);
    memcpy(result->m_chars + a->m_length, b->m_chars, b->m_length);
    result->m_hash = XXH3_64bits(result->m_chars, length);
    result->m_chars[length] = 0;
    return result;
}

void ObjString::free(ObjString* str) {
    str->~ObjString();
    ::free(str);
}

}
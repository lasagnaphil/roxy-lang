#pragma once

#include "roxy/object.hpp"

namespace rx {

struct ObjString {
private:
    Obj m_obj = ObjType::String;
    u64 m_hash;
    u32 m_length;
    char m_chars[];

public:
    const char* chars() const { return m_chars; }
    u32 length() const { return m_length; }
    u64 hash() const { return m_hash; }

    static ObjString* allocate(u32 length);
    static ObjString* create(const char* chars, u32 length);
    static ObjString* create_with_known_hash(const char* chars, u32 length, u64 hash);

    static ObjString* from_bool(bool value);
    static ObjString* from_i32(i32 value);
    static ObjString* from_i64(i64 value);
    static ObjString* from_u32(u32 value);
    static ObjString* from_u64(u64 value);
    static ObjString* from_f32(f32 value);
    static ObjString* from_f64(f64 value);

    static ObjString* concat(ObjString* a, ObjString* b);
    static void free(ObjString* str);
};

}
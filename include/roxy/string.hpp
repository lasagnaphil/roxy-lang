#pragma once

#include "roxy/value.hpp"

namespace rx {

struct ObjString {
    Obj obj = ObjType::String;
    u64 hash;
    u32 length;
    char chars[];
};

ObjString* allocate_obj_string(u32 length);

ObjString* create_obj_string(const char* chars, u32 length);

void free_obj_string(ObjString* obj_string);

ObjString* create_obj_string_with_known_hash(const char* chars, u32 length, u64 hash);

ObjString* concat_string(ObjString* a, ObjString* b);

}
#include "roxy/string_interner.hpp"

#include "roxy/core/xxhash.h"

// #define ROXY_DEBUG_ALLOCATIONS

namespace rx {

void StringInterner::init() {
    m_string_table.reserve(s_initial_table_capacity);
}

void StringInterner::free() {
    m_string_table.clear();
}

ObjString* StringInterner::create_string(std::string_view str) {
    return create_string(str.data(), (u32)str.length());
}

ObjString* StringInterner::create_string(const char *chars, u32 length) {
    u64 hash = XXH3_64bits(chars, length);
    return create_string(chars, length, hash);
}

ObjString* StringInterner::create_string(const char *chars, u32 length, u64 hash) {
    auto it = m_string_table.find(TableParam{chars, length, hash});
    if (it != m_string_table.end()) {
        return it.key();
    }
    else {
        ObjString* new_string = ObjString::create_with_known_hash(chars, length, hash);
        m_string_table.insert(new_string);
#ifdef ROXY_DEBUG_ALLOCATIONS
        printf("Allocated string \"%.*s\"\n", new_string->length(), new_string->chars());
        fflush(stdout);
#endif
        return new_string;
    }
}

ObjString* StringInterner::insert_existing_string_obj(ObjString* obj) {
    auto it = m_string_table.find(TableParam{obj->chars(), obj->length(), obj->hash()});
    if (it != m_string_table.end()) {
        // Found in intern pool, free the original object and return the interned object instead.
        obj->~ObjString();
        Obj::free(reinterpret_cast<Obj*>(obj));
        return it.key();
    }
    else {
        // Not found in intern pool, insert it to the pool and return the original object
        m_string_table.insert(obj);
#ifdef ROXY_DEBUG_ALLOCATIONS
        printf("Allocated string \"%.*s\"\n", obj->length(), obj->chars());
        fflush(stdout);
#endif
        return obj;
    }
}

void StringInterner::free_string(ObjString* str) {
    if (m_string_table.erase(str)) {
#ifdef ROXY_DEBUG_ALLOCATIONS
        printf("Freed string \"%.*s\"\n", str->length(), str->chars());
        fflush(stdout);
#endif
        str->~ObjString();
        // TODO: Disable freeing until we figure out the Debug CRT Crash (not a problem on Address Sanitizer though...)
        // Obj::free(reinterpret_cast<Obj*>(str));
    }
}

}
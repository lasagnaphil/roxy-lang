#include "roxy/string_interner.hpp"

#include "roxy/core/xxhash.h"

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
        ObjString* new_string = create_obj_string_with_known_hash(chars, length, hash);
        auto value = AnyValue(new_string);
        value.obj_incref();
        m_string_table.insert(new_string);
        return new_string;
    }
}

}
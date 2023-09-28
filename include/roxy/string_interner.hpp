#pragma once

#include "roxy/string.hpp"
#include "roxy/tsl/robin_set.h"

namespace rx {

class StringInterner {
public:
    void init();
    void free();

    ObjString* create_string(const char* chars, u32 length);
    ObjString* create_string(const char* chars, u32 length, u64 hash);

    void free_string(ObjString* str);

private:
    static constexpr u64 s_initial_table_capacity = 65536;

    struct TableParam {
        const char* chars;
        u32 length;
        u64 hash;
    };

    struct TableComparer {
        using is_transparent = void;

        bool operator()(const ObjString* key1, const ObjString* key2) const {
            return key1->length == key2->length && key1->hash == key2->hash &&
                memcmp(key1->chars, key2->chars, key1->length) == 0;
        }
        bool operator()(const ObjString* key, const TableParam& param) const {
            return key->length == param.length &&
                   memcmp(key->chars, param.chars, key->length) == 0;
        }
        bool operator()(const TableParam& param, const ObjString* key) const {
            return operator()(key, param);
        }
    };

    struct TableHasher {
        u64 operator()(const ObjString* key) const { return key->hash; }
        u64 operator()(const TableParam& param) const { return param.hash; }
    };

    tsl::robin_set<ObjString*, TableHasher, TableComparer> m_string_table;
};

}

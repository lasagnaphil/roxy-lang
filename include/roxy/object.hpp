#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/psuedorandom.hpp"

namespace rx {

enum class ObjType {
    Value, String,
};

inline thread_local xoshiro256ss_state tl_uid_gen_state;

void init_uid_gen_state();

struct Obj {
    u64 type_bits: 5;
    u64 uid: 59;
    u64 refcount; // TODO: make this atomic

    Obj(ObjType type) : type_bits((u64)type), refcount(1) {
        uid = xoshiro256ss(&tl_uid_gen_state);
    }

    ObjType type() const { return (ObjType)type_bits; }
};

struct ObjValue {
    Obj obj;
    u8 data[];

    ObjValue() : obj(ObjType::Value) {}
};

}
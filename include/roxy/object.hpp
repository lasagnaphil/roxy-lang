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
private:
    u64 m_type_bits: 5;
    u64 m_uid: 59;
    u64 m_refcount; // TODO: make this atomic

public:
    Obj(ObjType type) : m_type_bits((u64)type), m_refcount(1) {
        m_uid = xoshiro256ss(&tl_uid_gen_state);
    }

    ObjType type() const { return (ObjType)m_type_bits; }
};

struct ObjValue {
    Obj obj;
    u8 data[];

    ObjValue() : obj(ObjType::Value) {}
};

}
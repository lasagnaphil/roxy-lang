#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/psuedorandom.hpp"

#include <cstdlib>

namespace rx {

enum class ObjType {
    Value, String,
};

inline thread_local xoshiro256ss_state tl_uid_gen_state;

void init_uid_gen_state();

class ObjValue;
class ObjString;

class Obj {
    friend class ObjValue;
    friend class ObjString;
private:
    u64 m_type_bits: 5;
    u64 m_uid: 59;
    u64 m_refcount; // TODO: make this atomic
    void (*m_deleter) (Obj*);

public:
    Obj(ObjType type, void(*deleter)(Obj*)) : m_type_bits((u64)type), m_refcount(1), m_deleter(deleter) {
        m_uid = xoshiro256ss(&tl_uid_gen_state);
    }

    ObjType type() const { return (ObjType)m_type_bits; }
    u64 uid() const { return m_uid; }
    u64 refcount() const { return m_refcount; }

    void incref() { m_refcount++; }
    void decref() {
        m_refcount--;
        if (m_refcount == 0) m_deleter(this);
    }

    static void free(Obj* obj) {
        obj->m_uid = 0;
        ::free(obj);
    }
};

}
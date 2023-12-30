#pragma once

#include "roxy/object.hpp"

#include <new>

namespace rx {

class ObjValue {
private:
    Obj m_obj = {ObjType::Value, Obj::free};
    u8 m_data[1];

public:
    ObjValue() {}

    Obj& obj() { return m_obj; }
    const Obj& obj() const { return m_obj; }

    static ObjValue* allocate(u32 size) {
        void* raw_data = malloc(offsetof(ObjValue, m_data[size + 1]));
        new (raw_data) ObjValue();
        auto value = static_cast<ObjValue*>(raw_data);
        return value;
    }
};

}
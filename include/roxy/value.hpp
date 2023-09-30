#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/psuedorandom.hpp"

#include <type_traits>
#include <memory>
#include <string>

namespace rx {

enum class ValueType {
    Bool, Nil, Number, Obj
};

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

struct ObjString;

struct AnyValue {

#define SIGN_BIT ((uint64_t)0x8000000000000000)
#define QNAN ((uint64_t)0x7ffc000000000000)
#define TAG_NIL 1
#define TAG_FALSE 2
#define TAG_TRUE 3

#define NIL_VAL         ((uint64_t)(QNAN | TAG_NIL))
#define FALSE_VAL       ((uint64_t)(QNAN | TAG_FALSE))
#define TRUE_VAL        ((uint64_t)(QNAN | TAG_TRUE))

    uint64_t value;

    AnyValue() : value((uint64_t)(QNAN | TAG_NIL)) {}

    // Need to do this template shenanigans to prevent any T* -> bool implicit conversions (C++ wtf)
    template <typename T, typename = std::enable_if_t<std::is_same_v<T, bool>>>
    explicit AnyValue(T boolean) : value(boolean ? TRUE_VAL : FALSE_VAL) {}

    explicit AnyValue(double number) {
        memcpy(&value, &number, sizeof(AnyValue));
    }
    explicit AnyValue(Obj* obj) {
        value = SIGN_BIT | QNAN | (uint64_t)(uintptr_t)(obj);
    }

    bool is_bool() const { return (value | 1) == TRUE_VAL; }
    bool is_nil() const { return value == NIL_VAL; }
    bool is_number() const { return (value & QNAN) != QNAN; }
    bool is_obj() const { return (value & (QNAN | SIGN_BIT)) == (QNAN | SIGN_BIT); }
    bool is_obj_type(ObjType type) const { return is_obj() && as_obj()->type() == type; }

    bool as_bool() const { return value == TRUE_VAL; }
    double as_number() const {
        double num;
        memcpy(&num, &value, sizeof(AnyValue));
        return num;
    }
    Obj* as_obj() const { return (Obj*)(uintptr_t)(value & ~(SIGN_BIT | QNAN)); }

    explicit AnyValue(ObjString* obj) : AnyValue(reinterpret_cast<Obj*>(obj)) {}

    bool is_string() const { return is_obj_type(ObjType::String); }

    ObjString* as_string() const { return reinterpret_cast<ObjString*>(as_obj()); }

    ObjType obj_type() const { return as_obj()->type(); }

    bool is_falsey() const {
        return is_nil() || (is_bool() && !as_bool());
    }

    void obj_incref() {
        as_obj()->refcount++;
    }

    void obj_decref() {
        Obj* obj = as_obj();
        obj->refcount--;
        if (obj->refcount == 0) {
            obj_free();
            // After this, the AnyValue is in an invalid state, don't use it!
        }
    }

    void obj_free();

    u64 hash() const;

    static bool equals(const AnyValue& a, const AnyValue& b) { return a.value == b.value; }

    std::string to_std_string(bool print_refcount = false) const;
};

}

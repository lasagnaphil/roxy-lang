#pragma once

#include "roxy/core/types.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/vm/value.hpp"

#include <cstring>

namespace rx {

// Primary template - undefined to cause compile error for unsupported types
template<typename T>
struct RoxyType;

// void specialization
template<>
struct RoxyType<void> {
    static Type* get(TypeCache& tc) { return tc.void_type(); }
    // No from_value/to_value or from_reg/to_reg for void
};

// bool specialization
template<>
struct RoxyType<bool> {
    static Type* get(TypeCache& tc) { return tc.bool_type(); }
    static bool from_value(const Value& v) { return v.as_bool; }
    static Value to_value(bool b) { return Value::make_bool(b); }
    static bool from_reg(u64 r) { return r != 0; }
    static u64 to_reg(bool b) { return b ? 1 : 0; }
};

// i8 specialization
template<>
struct RoxyType<i8> {
    static Type* get(TypeCache& tc) { return tc.i8_type(); }
    static i8 from_value(const Value& v) { return static_cast<i8>(v.as_int); }
    static Value to_value(i8 val) { return Value::make_int(val); }
    static i8 from_reg(u64 r) { return static_cast<i8>(r); }
    static u64 to_reg(i8 val) { return static_cast<u64>(static_cast<i64>(val)); }
};

// i16 specialization
template<>
struct RoxyType<i16> {
    static Type* get(TypeCache& tc) { return tc.i16_type(); }
    static i16 from_value(const Value& v) { return static_cast<i16>(v.as_int); }
    static Value to_value(i16 val) { return Value::make_int(val); }
    static i16 from_reg(u64 r) { return static_cast<i16>(r); }
    static u64 to_reg(i16 val) { return static_cast<u64>(static_cast<i64>(val)); }
};

// i32 specialization
template<>
struct RoxyType<i32> {
    static Type* get(TypeCache& tc) { return tc.i32_type(); }
    static i32 from_value(const Value& v) { return static_cast<i32>(v.as_int); }
    static Value to_value(i32 val) { return Value::make_int(val); }
    static i32 from_reg(u64 r) { return static_cast<i32>(r); }
    static u64 to_reg(i32 val) { return static_cast<u64>(static_cast<i64>(val)); }
};

// i64 specialization
template<>
struct RoxyType<i64> {
    static Type* get(TypeCache& tc) { return tc.i64_type(); }
    static i64 from_value(const Value& v) { return v.as_int; }
    static Value to_value(i64 val) { return Value::make_int(val); }
    static i64 from_reg(u64 r) { return static_cast<i64>(r); }
    static u64 to_reg(i64 val) { return static_cast<u64>(val); }
};

// u8 specialization
template<>
struct RoxyType<u8> {
    static Type* get(TypeCache& tc) { return tc.u8_type(); }
    static u8 from_value(const Value& v) { return static_cast<u8>(v.as_int); }
    static Value to_value(u8 val) { return Value::make_int(val); }
    static u8 from_reg(u64 r) { return static_cast<u8>(r); }
    static u64 to_reg(u8 val) { return static_cast<u64>(val); }
};

// u16 specialization
template<>
struct RoxyType<u16> {
    static Type* get(TypeCache& tc) { return tc.u16_type(); }
    static u16 from_value(const Value& v) { return static_cast<u16>(v.as_int); }
    static Value to_value(u16 val) { return Value::make_int(val); }
    static u16 from_reg(u64 r) { return static_cast<u16>(r); }
    static u64 to_reg(u16 val) { return static_cast<u64>(val); }
};

// u32 specialization
template<>
struct RoxyType<u32> {
    static Type* get(TypeCache& tc) { return tc.u32_type(); }
    static u32 from_value(const Value& v) { return static_cast<u32>(v.as_int); }
    static Value to_value(u32 val) { return Value::make_int(val); }
    static u32 from_reg(u64 r) { return static_cast<u32>(r); }
    static u64 to_reg(u32 val) { return static_cast<u64>(val); }
};

// u64 specialization
template<>
struct RoxyType<u64> {
    static Type* get(TypeCache& tc) { return tc.u64_type(); }
    static u64 from_value(const Value& v) { return static_cast<u64>(v.as_int); }
    static Value to_value(u64 val) { return Value::make_int(static_cast<i64>(val)); }
    static u64 from_reg(u64 r) { return r; }
    static u64 to_reg(u64 val) { return val; }
};

// f32 specialization
template<>
struct RoxyType<f32> {
    static Type* get(TypeCache& tc) { return tc.f32_type(); }
    static f32 from_value(const Value& v) { return static_cast<f32>(v.as_float); }
    static Value to_value(f32 val) { return Value::make_float(val); }
    static f32 from_reg(u64 r) {
        // f32 is stored as f64 bits in register
        f64 d;
        memcpy(&d, &r, sizeof(d));
        return static_cast<f32>(d);
    }
    static u64 to_reg(f32 val) {
        f64 d = static_cast<f64>(val);
        u64 r;
        memcpy(&r, &d, sizeof(r));
        return r;
    }
};

// f64 specialization
template<>
struct RoxyType<f64> {
    static Type* get(TypeCache& tc) { return tc.f64_type(); }
    static f64 from_value(const Value& v) { return v.as_float; }
    static Value to_value(f64 val) { return Value::make_float(val); }
    static f64 from_reg(u64 r) {
        f64 d;
        memcpy(&d, &r, sizeof(d));
        return d;
    }
    static u64 to_reg(f64 val) {
        u64 r;
        memcpy(&r, &val, sizeof(r));
        return r;
    }
};

}

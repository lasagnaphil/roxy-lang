#pragma once

#include "roxy/core/types.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/vm/value.hpp"

namespace rx {

// Primary template - undefined to cause compile error for unsupported types
template<typename T>
struct RoxyType;

// void specialization
template<>
struct RoxyType<void> {
    static Type* get(TypeCache& tc) { return tc.void_type(); }
    // No from_value/to_value for void
};

// bool specialization
template<>
struct RoxyType<bool> {
    static Type* get(TypeCache& tc) { return tc.bool_type(); }
    static bool from_value(const Value& v) { return v.as_bool; }
    static Value to_value(bool b) { return Value::make_bool(b); }
};

// i8 specialization
template<>
struct RoxyType<i8> {
    static Type* get(TypeCache& tc) { return tc.i8_type(); }
    static i8 from_value(const Value& v) { return static_cast<i8>(v.as_int); }
    static Value to_value(i8 val) { return Value::make_int(val); }
};

// i16 specialization
template<>
struct RoxyType<i16> {
    static Type* get(TypeCache& tc) { return tc.i16_type(); }
    static i16 from_value(const Value& v) { return static_cast<i16>(v.as_int); }
    static Value to_value(i16 val) { return Value::make_int(val); }
};

// i32 specialization
template<>
struct RoxyType<i32> {
    static Type* get(TypeCache& tc) { return tc.i32_type(); }
    static i32 from_value(const Value& v) { return static_cast<i32>(v.as_int); }
    static Value to_value(i32 val) { return Value::make_int(val); }
};

// i64 specialization
template<>
struct RoxyType<i64> {
    static Type* get(TypeCache& tc) { return tc.i64_type(); }
    static i64 from_value(const Value& v) { return v.as_int; }
    static Value to_value(i64 val) { return Value::make_int(val); }
};

// u8 specialization
template<>
struct RoxyType<u8> {
    static Type* get(TypeCache& tc) { return tc.u8_type(); }
    static u8 from_value(const Value& v) { return static_cast<u8>(v.as_int); }
    static Value to_value(u8 val) { return Value::make_int(val); }
};

// u16 specialization
template<>
struct RoxyType<u16> {
    static Type* get(TypeCache& tc) { return tc.u16_type(); }
    static u16 from_value(const Value& v) { return static_cast<u16>(v.as_int); }
    static Value to_value(u16 val) { return Value::make_int(val); }
};

// u32 specialization
template<>
struct RoxyType<u32> {
    static Type* get(TypeCache& tc) { return tc.u32_type(); }
    static u32 from_value(const Value& v) { return static_cast<u32>(v.as_int); }
    static Value to_value(u32 val) { return Value::make_int(val); }
};

// u64 specialization
template<>
struct RoxyType<u64> {
    static Type* get(TypeCache& tc) { return tc.u64_type(); }
    static u64 from_value(const Value& v) { return static_cast<u64>(v.as_int); }
    static Value to_value(u64 val) { return Value::make_int(static_cast<i64>(val)); }
};

// f32 specialization
template<>
struct RoxyType<f32> {
    static Type* get(TypeCache& tc) { return tc.f32_type(); }
    static f32 from_value(const Value& v) { return static_cast<f32>(v.as_float); }
    static Value to_value(f32 val) { return Value::make_float(val); }
};

// f64 specialization
template<>
struct RoxyType<f64> {
    static Type* get(TypeCache& tc) { return tc.f64_type(); }
    static f64 from_value(const Value& v) { return v.as_float; }
    static Value to_value(f64 val) { return Value::make_float(val); }
};

}

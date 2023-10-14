#pragma once

#include "roxy/core/types.hpp"
#include "roxy/type.hpp"

#include <type_traits>
#include <memory>
#include <string>

namespace rx {

struct Obj;
struct ObjString;

struct AnyValue {
    PrimTypeKind kind;
    bool is_boxed;
    union {
        bool value_bool;
        u8 value_u8;
        u16 value_u16;
        u32 value_u32;
        u64 value_u64;
        i8 value_i8;
        i16 value_i16;
        i32 value_i32;
        i64 value_i64;
        f32 value_f32;
        f64 value_f64;
        const char* str;
    };

    AnyValue() : kind(PrimTypeKind::Void) {}

    template <typename T, typename = std::enable_if_t<std::is_same_v<T, bool>>>
    explicit AnyValue(T boolean) : kind(PrimTypeKind::Bool), is_boxed(false), value_bool(boolean) {}

    explicit AnyValue(u8 value)  : kind(PrimTypeKind::U8 ), is_boxed(false), value_u8 (value) {}
    explicit AnyValue(u16 value) : kind(PrimTypeKind::U16), is_boxed(false), value_u16(value) {}
    explicit AnyValue(u32 value) : kind(PrimTypeKind::U32), is_boxed(false), value_u32(value) {}
    explicit AnyValue(u64 value) : kind(PrimTypeKind::U64), is_boxed(false), value_u64(value) {}

    explicit AnyValue(i8 value)  : kind(PrimTypeKind::I8 ), is_boxed(false), value_i8 (value) {}
    explicit AnyValue(i16 value) : kind(PrimTypeKind::I16), is_boxed(false), value_i16(value) {}
    explicit AnyValue(i32 value) : kind(PrimTypeKind::I32), is_boxed(false), value_i32(value) {}
    explicit AnyValue(i64 value) : kind(PrimTypeKind::I64), is_boxed(false), value_i64(value) {}

    explicit AnyValue(f32 value) : kind(PrimTypeKind::F32), is_boxed(false), value_f32(value) {}
    explicit AnyValue(f64 value) : kind(PrimTypeKind::F64), is_boxed(false), value_f64(value) {}

    explicit AnyValue(const char* str) : kind(PrimTypeKind::String), is_boxed(true), str(str) {}

    std::string to_std_string(bool print_refcount = false) const;
};

}

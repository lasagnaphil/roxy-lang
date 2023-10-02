#include "roxy/value.hpp"
#include "roxy/core/xxhash.h"
#include "roxy/fmt/core.h"
#include "roxy/string.hpp"

namespace rx {

std::string object_to_string(AnyValue value, bool print_refcount);

std::string value_to_string(AnyValue value, bool print_refcount) {
    switch (value.kind) {
        case PrimTypeKind::Void: return "nil";
        case PrimTypeKind::Bool: return value.value_bool? "true" : "false";
        case PrimTypeKind::I8: return std::to_string(value.value_i8);
        case PrimTypeKind::I16: return std::to_string(value.value_i16);
        case PrimTypeKind::I32: return std::to_string(value.value_i32);
        case PrimTypeKind::I64: return std::to_string(value.value_i64);
        case PrimTypeKind::U8: return std::to_string(value.value_u8);
        case PrimTypeKind::U16: return std::to_string(value.value_u16);
        case PrimTypeKind::U32: return std::to_string(value.value_u32);
        case PrimTypeKind::U64: return std::to_string(value.value_u64);
        case PrimTypeKind::String: {
            if (print_refcount)
                return fmt::format("{} ({})", object_to_string(value, true), value.obj->refcount);
            else
                return object_to_string(value, false);

        }
    }
    return "";
}

std::string object_to_string(AnyValue value, bool print_refcount) {
    switch (value.obj->type()) {
        case ObjType::String: return fmt::format("\"{}\"", reinterpret_cast<ObjString*>(value.obj)->chars);
        default: return "";
    }
}

std::string AnyValue::to_std_string(bool print_refcount) const {
    return value_to_string(*this, print_refcount);
}

}
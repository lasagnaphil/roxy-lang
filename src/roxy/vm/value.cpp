#include "roxy/vm/value.hpp"
#include "roxy/vm/object.hpp"

#include <cstdio>

namespace rx {

ObjectHeader* Value::get_object_header() const {
    if (type == Ptr && as_ptr != nullptr) {
        return get_header_from_data(as_ptr);
    }
    if (type == Weak && as_weak.ptr != nullptr) {
        return get_header_from_data(as_weak.ptr);
    }
    return nullptr;
}

bool Value::is_weak_valid() const {
    if (type != Weak) return false;
    if (as_weak.ptr == nullptr) return false;
    return weak_ref_valid(as_weak.ptr, as_weak.generation);
}

const char* value_type_to_string(Value::Type type) {
    switch (type) {
        case Value::Null:   return "null";
        case Value::Bool:   return "bool";
        case Value::Int:    return "int";
        case Value::Float:  return "float";
        case Value::Ptr:    return "ptr";
        case Value::Weak:   return "weak";
        default:            return "unknown";
    }
}

void value_to_string(const Value& value, char* buf, u32 buf_size) {
    switch (value.type) {
        case Value::Null:
            snprintf(buf, buf_size, "null");
            break;
        case Value::Bool:
            snprintf(buf, buf_size, "%s", value.as_bool ? "true" : "false");
            break;
        case Value::Int:
            snprintf(buf, buf_size, "%lld", static_cast<long long>(value.as_int));
            break;
        case Value::Float:
            snprintf(buf, buf_size, "%g", value.as_float);
            break;
        case Value::Ptr:
            snprintf(buf, buf_size, "ptr(%p)", value.as_ptr);
            break;
        case Value::Weak:
            snprintf(buf, buf_size, "weak(%p, gen=%llu)", value.as_weak.ptr,
                     static_cast<unsigned long long>(value.as_weak.generation));
            break;
        default:
            snprintf(buf, buf_size, "unknown");
            break;
    }
}

}

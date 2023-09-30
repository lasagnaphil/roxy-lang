#include "roxy/value.hpp"
#include "roxy/core/xxhash.h"
#include "roxy/fmt/core.h"
#include "roxy/string.hpp"

#include <ctime>

namespace rx {

void init_uid_gen_state() {
    u64 t = time(nullptr);
    xoshiro256ss_init(&tl_uid_gen_state, t);
}

void AnyValue::obj_free() {
    Obj* obj = as_obj();
    switch (obj->type()) {
        case ObjType::String: {
            free_obj_string(reinterpret_cast<ObjString*>(obj));
            break;
        }
    }
    // Zero-out the pointer for some safety
    value = (QNAN | SIGN_BIT);
}

u64 AnyValue::hash() const {
    if (is_nil()) return 0;
    else if (is_bool()) return as_bool()? 1231 : 1237;
    else if (is_number()) {
        double number = as_number();
        auto bits = reinterpret_cast<const char*>(&number);
        return XXH3_64bits(bits, 4);
    }
    else if (is_obj()) {
        Obj* obj = as_obj();
        if (obj->type() == ObjType::String) {
            return reinterpret_cast<ObjString*>(obj)->hash;
        }
        else {
            auto bits = reinterpret_cast<const char *>(&obj);
            return XXH3_64bits(bits, 4);
        }
    }
    return 0;
}

std::string object_to_string(AnyValue value, bool print_refcount);

std::string value_to_string(AnyValue value, bool print_refcount) {
    if (value.is_bool()) return value.as_bool()? "true" : "false";
    else if (value.is_nil()) return "nil";
    else if (value.is_number()) return fmt::format("{:g}", value.as_number());
    else if (value.is_obj()) {
        if (print_refcount)
            return fmt::format("{} ({})", object_to_string(value, true), value.as_obj()->refcount);
        else
            return object_to_string(value, false);

    }
    else return "";
}

std::string object_to_string(AnyValue value, bool print_refcount) {
    Obj* obj = value.as_obj();
    switch (obj->type()) {
        case ObjType::String: return fmt::format("\"{}\"", value.as_string()->chars);
        default: return "";
    }
}

std::string AnyValue::to_std_string(bool print_refcount) const {
    return value_to_string(*this, print_refcount);
}

}
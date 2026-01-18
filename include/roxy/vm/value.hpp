#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Forward declarations
struct ObjectHeader;

// Runtime value representation - tagged union
// Uses NaN-boxing for efficient storage (8 bytes total)
// However, for clarity and simplicity, we use an explicit tagged union here.
struct Value {
    enum Type : u8 {
        Null,       // null value
        Bool,       // boolean value
        Int,        // 64-bit signed integer
        Float,      // 64-bit floating point
        Ptr,        // pointer to object (uniq or ref)
        Weak,       // weak pointer with generation check
    };

    Type type;
    union {
        bool as_bool;
        i64 as_int;
        f64 as_float;
        void* as_ptr;
        struct {
            void* ptr;
            u32 generation;
        } as_weak;
    };

    // Default constructor - creates null value
    Value() : type(Null), as_int(0) {}

    // Factory methods for creating values
    static Value make_null() {
        Value v;
        v.type = Null;
        v.as_int = 0;
        return v;
    }

    static Value make_bool(bool b) {
        Value v;
        v.type = Bool;
        v.as_bool = b;
        return v;
    }

    static Value make_int(i64 i) {
        Value v;
        v.type = Int;
        v.as_int = i;
        return v;
    }

    static Value make_float(f64 f) {
        Value v;
        v.type = Float;
        v.as_float = f;
        return v;
    }

    static Value make_ptr(void* p) {
        Value v;
        v.type = Ptr;
        v.as_ptr = p;
        return v;
    }

    static Value make_weak(void* p, u32 generation) {
        Value v;
        v.type = Weak;
        v.as_weak.ptr = p;
        v.as_weak.generation = generation;
        return v;
    }

    // Type checks
    bool is_null() const { return type == Null; }
    bool is_bool() const { return type == Bool; }
    bool is_int() const { return type == Int; }
    bool is_float() const { return type == Float; }
    bool is_ptr() const { return type == Ptr; }
    bool is_weak() const { return type == Weak; }
    bool is_object() const { return type == Ptr || type == Weak; }

    // Truthiness (for conditionals)
    bool is_truthy() const {
        switch (type) {
            case Null: return false;
            case Bool: return as_bool;
            case Int: return as_int != 0;
            case Float: return as_float != 0.0;
            case Ptr: return as_ptr != nullptr;
            case Weak: return as_weak.ptr != nullptr;
            default: return false;
        }
    }

    // Get object header from pointer types
    ObjectHeader* get_object_header() const;

    // Check if weak reference is still valid
    bool is_weak_valid() const;

    // Equality comparison
    bool equals(const Value& other) const {
        if (type != other.type) return false;
        switch (type) {
            case Null: return true;
            case Bool: return as_bool == other.as_bool;
            case Int: return as_int == other.as_int;
            case Float: return as_float == other.as_float;
            case Ptr: return as_ptr == other.as_ptr;
            case Weak: return as_weak.ptr == other.as_weak.ptr &&
                              as_weak.generation == other.as_weak.generation;
            default: return false;
        }
    }

    bool operator==(const Value& other) const { return equals(other); }
    bool operator!=(const Value& other) const { return !equals(other); }

    // Convert value to raw u64 for register storage
    // For Int/Ptr types, stores the raw bits
    // For Float, uses type punning to store the bits
    u64 as_u64() const {
        switch (type) {
            case Bool: return as_bool ? 1 : 0;
            case Int: return static_cast<u64>(as_int);
            case Float: {
                u64 bits;
                __builtin_memcpy(&bits, &as_float, sizeof(bits));
                return bits;
            }
            case Ptr: return reinterpret_cast<u64>(as_ptr);
            case Weak: return reinterpret_cast<u64>(as_weak.ptr);
            default: return 0;
        }
    }

    // Create Value from raw u64 register value
    // Assumes the value is an integer (default interpretation)
    static Value from_u64(u64 bits) {
        return make_int(static_cast<i64>(bits));
    }

    // Create float Value from raw u64 bits
    static Value float_from_u64(u64 bits) {
        f64 f;
        __builtin_memcpy(&f, &bits, sizeof(f));
        return make_float(f);
    }

    // Create pointer Value from raw u64 bits
    static Value ptr_from_u64(u64 bits) {
        return make_ptr(reinterpret_cast<void*>(bits));
    }
};

// Type name for debugging
const char* value_type_to_string(Value::Type type);

// String representation of a value (for debugging)
void value_to_string(const Value& value, char* buf, u32 buf_size);

}

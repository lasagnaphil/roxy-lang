#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/binding/type_traits.hpp"

#include <cstring>

namespace rx {

// Forward declaration
struct RoxyVM;

// RoxyString - thin non-owning wrapper around string data pointer (void*)
// Provides type-safe access to Roxy strings from C++ bound functions.
struct RoxyString {
    // Factory: allocate a new string via the VM
    static RoxyString alloc(RoxyVM* vm, const char* data, u32 length) {
        void* str_data = string_alloc(vm, data, length);
        return RoxyString(str_data);
    }

    // Convenience overload using strlen
    static RoxyString alloc(RoxyVM* vm, const char* data) {
        return alloc(vm, data, static_cast<u32>(std::strlen(data)));
    }

    explicit RoxyString(void* data) : m_data(data) {}

    // String length (excluding null terminator)
    u32 length() const { return string_length(m_data); }

    // Null-terminated character data
    const char* c_str() const { return string_chars(m_data); }

    // Equality comparison
    bool equals(const RoxyString& other) const {
        return string_equals(m_data, other.m_data);
    }

    // Concatenate two strings, returns a new RoxyString
    RoxyString concat(RoxyVM* vm, RoxyString other) const {
        void* result = string_concat(vm, m_data, other.m_data);
        return RoxyString(result);
    }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

// RoxyType specialization for RoxyString
template<>
struct RoxyType<RoxyString> {
    static Type* get(TypeCache& tc) {
        return tc.string_type();
    }
    static RoxyString from_reg(u64 r) {
        return RoxyString(reinterpret_cast<void*>(r));
    }
    static u64 to_reg(RoxyString str) {
        return reinterpret_cast<u64>(str.data());
    }
};

}

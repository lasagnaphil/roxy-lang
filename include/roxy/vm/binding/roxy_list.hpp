#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/value.hpp"
#include "roxy/vm/binding/type_traits.hpp"

#include <cassert>

namespace rx {

// Forward declaration
struct RoxyVM;

// RoxyList<T> - thin non-owning typed wrapper around list data pointer (void*)
// Provides type-safe access to Roxy lists from C++ bound functions.
template<typename T>
struct RoxyList {
    // Factory: allocate a new list via the VM
    static RoxyList<T> alloc(RoxyVM* vm, u32 capacity) {
        void* data = list_alloc(vm, capacity);
        return RoxyList<T>(data);
    }

    explicit RoxyList(void* data) : m_data(data) {}

    // Element access (bounds-checked)
    T get(i64 index) const {
        Value val;
        const char* error = nullptr;
        bool ok = list_get(m_data, index, val, &error);
        assert(ok && "list_get failed");
        (void)ok;
        return RoxyType<T>::from_reg(val.as_u64());
    }

    void set(i64 index, T value) {
        Value val = Value::from_u64(RoxyType<T>::to_reg(value));
        const char* error = nullptr;
        bool ok = list_set(m_data, index, val, &error);
        assert(ok && "list_set failed");
        (void)ok;
    }

    void push(T value) {
        Value val = Value::from_u64(RoxyType<T>::to_reg(value));
        list_push(m_data, val);
    }

    T pop() {
        Value val = list_pop(m_data);
        return RoxyType<T>::from_reg(val.as_u64());
    }

    u32 len() const { return list_length(m_data); }
    u32 cap() const { return list_capacity(m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

// RoxyType specialization for RoxyList<T>
template<typename T>
struct RoxyType<RoxyList<T>> {
    static Type* get(TypeCache& tc) {
        return tc.list_type(RoxyType<T>::get(tc));
    }
    static RoxyList<T> from_reg(u64 r) {
        return RoxyList<T>(reinterpret_cast<void*>(r));
    }
    static u64 to_reg(RoxyList<T> list) {
        return reinterpret_cast<u64>(list.data());
    }
};

}

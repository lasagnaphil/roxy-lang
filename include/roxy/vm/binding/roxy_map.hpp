#pragma once

#include "roxy/core/types.hpp"
#include "roxy/vm/map.hpp"
#include "roxy/vm/binding/type_traits.hpp"

#include <cassert>

namespace rx {

// Forward declaration
struct RoxyVM;

// RoxyMap<K, V> - thin non-owning typed wrapper around map data pointer (void*)
// Provides type-safe access to Roxy maps from C++ bound functions.
template<typename K, typename V>
struct RoxyMap {
    // Factory: allocate a new map via the VM
    static RoxyMap<K, V> alloc(RoxyVM* vm, MapKeyKind key_kind, u32 capacity = 0) {
        void* data = map_alloc(vm, key_kind, capacity);
        return RoxyMap<K, V>(data);
    }

    explicit RoxyMap(void* data) : m_data(data) {}

    V get(K key) const {
        u64 out;
        const char* error = nullptr;
        bool ok = map_get(m_data, RoxyType<K>::to_reg(key), out, &error);
        assert(ok && "map_get failed");
        (void)ok;
        return RoxyType<V>::from_reg(out);
    }

    void insert(K key, V value) {
        map_insert(m_data, RoxyType<K>::to_reg(key), RoxyType<V>::to_reg(value));
    }

    bool contains(K key) const {
        return map_contains(m_data, RoxyType<K>::to_reg(key));
    }

    bool remove(K key) {
        return map_remove(m_data, RoxyType<K>::to_reg(key));
    }

    void clear() { map_clear(m_data); }
    u32 len() const { return map_length(m_data); }

    bool is_valid() const { return m_data != nullptr; }
    void* data() const { return m_data; }

private:
    void* m_data;
};

// RoxyType specialization for RoxyMap<K, V>
template<typename K, typename V>
struct RoxyType<RoxyMap<K, V>> {
    static Type* get(TypeCache& tc) {
        return tc.map_type(RoxyType<K>::get(tc), RoxyType<V>::get(tc));
    }
    static RoxyMap<K, V> from_reg(u64 r) {
        return RoxyMap<K, V>(reinterpret_cast<void*>(r));
    }
    static u64 to_reg(RoxyMap<K, V> map) {
        return reinterpret_cast<u64>(map.data());
    }
};

}

#pragma once

#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"
#include "roxy/vm/map.hpp"
#include "roxy/vm/binding/type_traits.hpp"

namespace rx {

// Forward declaration
struct RoxyVM;

// `RoxyMap<K, V>` is a thin alias of `roxy::Map<K, V>`. See roxy_string.hpp
// for the rationale on aliasing — the unified runtime owns the
// implementation.
template<typename K, typename V>
using RoxyMap = roxy::Map<K, V>;

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

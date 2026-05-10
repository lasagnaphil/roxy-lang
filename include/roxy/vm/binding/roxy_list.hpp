#pragma once

#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"
#include "roxy/vm/value.hpp"
#include "roxy/vm/binding/type_traits.hpp"

namespace rx {

// Forward declaration
struct RoxyVM;

// `RoxyList<T>` is a thin alias of `roxy::List<T>`. See roxy_string.hpp for
// the rationale on aliasing — the unified runtime owns the implementation.
template<typename T>
using RoxyList = roxy::List<T>;

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

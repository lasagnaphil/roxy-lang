#pragma once

#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"
#include "roxy/vm/binding/type_traits.hpp"

namespace rx {

// Forward declaration
struct RoxyVM;

// `RoxyString` is a thin alias of `roxy::String`. The unified runtime now
// owns the implementation; this alias keeps existing embedder code that
// referenced `rx::RoxyString` working unchanged. Allocation flows through
// the active `roxy_ctx`'s allocator (slab in VM mode, slab in AOT mode
// after `roxy_rt_init`), so callers no longer need to thread a `RoxyVM*`.
using RoxyString = roxy::String;

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

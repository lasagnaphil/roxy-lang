#include "roxy/vm/string.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/rt/roxy_rt.h"

namespace rx {

// Global string type ID (registered once at startup)
static u32 g_string_type_id = UINT32_MAX;

u32 register_string_type() {
    if (g_string_type_id == UINT32_MAX) {
        g_string_type_id = register_object_type("string", 0, nullptr);
    }
    return g_string_type_id;
}

u32 get_string_type_id() {
    return g_string_type_id;
}

// `vm_call_index` activates `vm->ctx` via ScopedContext, which carries the
// allocator vtable + intern table the runtime needs. So these wrappers can
// drop the `RoxyVM*` parameter and route directly to `roxy_rt`.
//
// The `vm` parameter is retained for source-level compatibility with the
// pre-unification API; once Phase 6 lands, native callers can switch to the
// ctx-free `roxy_*` functions directly.

void* string_alloc(RoxyVM* /*vm*/, const char* data, u32 length) {
    return roxy_string_from_literal(data, length);
}

void* string_concat(RoxyVM* /*vm*/, void* str1, void* str2) {
    return roxy_string_concat(str1, str2);
}

bool string_equals(const void* str1, const void* str2) {
    // `roxy_string_eq` takes non-const void*; the underlying impl never
    // mutates, so casting away constness is safe.
    return roxy_string_eq(const_cast<void*>(str1), const_cast<void*>(str2));
}

}

#include "roxy/vm/map.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/map_dispatch.hpp"
#include "roxy/rt/roxy_rt.h"

#include <cstring>
#include <cassert>

namespace rx {

// Global map type ID (registered once at startup)
static u32 g_map_type_id = UINT32_MAX;

u32 register_map_type() {
    if (g_map_type_id == UINT32_MAX) {
        g_map_type_id = register_object_type("map", 0, nullptr);
    }
    return g_map_type_id;
}

u32 get_map_type_id() {
    return g_map_type_id;
}

// VM-side map ops are now thin wrappers around the unified `roxy_map_*`
// runtime. Custom user-defined Hash/Eq dispatch (Struct keys with `impl Hash`
// / `impl Eq`) routes through a thread-local dispatch frame: each public
// VM op pushes a `MapDispatchFrame` carrying `(vm, hash_fn_idx, eq_fn_idx,
// key_slot_count)` before calling into `roxy_map_*`, and pops on return.
// The hash/eq trampolines installed in `MapHeader.hash_fn`/`eq_fn` read the
// top of the stack and re-enter the interpreter via `call_user_function`.

namespace {

// RAII guard around `map_dispatch_push`/`pop`.
struct MapDispatchScope {
    MapDispatchScope(RoxyVM* vm, const MapHeader* header) {
        MapDispatchFrame f;
        f.vm = vm;
        f.hash_fn_idx = header->hash_fn_index;
        f.eq_fn_idx = header->eq_fn_index;
        f.key_slot_count = header->key_slot_count;
        map_dispatch_push(f);
    }
    ~MapDispatchScope() { map_dispatch_pop(); }
    MapDispatchScope(const MapDispatchScope&) = delete;
    MapDispatchScope& operator=(const MapDispatchScope&) = delete;
};

}

// --- Public API ---

void* map_alloc(RoxyVM* /*vm*/, MapKeyKind key_kind, u32 capacity,
                u8 key_slot_count, bool key_is_inline,
                u8 value_slot_count, bool value_is_inline,
                u32 hash_fn_index, u32 eq_fn_index) {
    // Install the VM trampolines whenever the user provided custom Hash/Eq
    // bytecode impls. AOT mode would write actual function pointers here;
    // VM mode points at the trampolines, which read the dispatch stack
    // for the active (vm, fn_idx) pair.
    roxy_map_hash_fn hash_fn = (hash_fn_index != UINT32_MAX)
        ? map_dispatch_hash_trampoline() : nullptr;
    roxy_map_eq_fn eq_fn = (eq_fn_index != UINT32_MAX)
        ? map_dispatch_eq_trampoline() : nullptr;

    void* data = roxy_map_alloc(static_cast<int32_t>(key_slot_count > 0 ? key_slot_count : 2),
                                key_is_inline ? 1 : 0,
                                static_cast<int32_t>(value_slot_count > 0 ? value_slot_count : 2),
                                value_is_inline ? 1 : 0,
                                hash_fn, eq_fn);
    if (!data) return nullptr;
    roxy_map_init(data, static_cast<int32_t>(key_kind), static_cast<int32_t>(capacity));

    MapHeader* header = get_map_header(data);
    header->hash_fn_index = hash_fn_index;
    header->eq_fn_index = eq_fn_index;
    return data;
}

void* map_copy(RoxyVM* vm, void* src) {
    if (!src) return nullptr;
    MapDispatchScope scope(vm, get_map_header(src));
    void* dst = roxy_map_copy(src);
    if (!dst) return nullptr;
    // `roxy_map_copy` preserves `hash_fn`/`eq_fn` and resets the index
    // fields to UINT32_MAX. Restore the bytecode indices so the dst can
    // dispatch through the VM trampolines just like the source.
    const MapHeader* src_header = get_map_header(src);
    MapHeader* dst_header = get_map_header(dst);
    dst_header->hash_fn_index = src_header->hash_fn_index;
    dst_header->eq_fn_index = src_header->eq_fn_index;
    return dst;
}

bool map_contains(RoxyVM* vm, const void* data, const u32* key_src) {
    if (!data) return false;
    MapDispatchScope scope(vm, get_map_header(data));
    return roxy_map_contains(const_cast<void*>(data), key_src);
}

const u32* map_get_ptr(RoxyVM* vm, const void* data, const u32* key_src, const char** error) {
    if (data == nullptr) {
        *error = "Null map reference";
        return nullptr;
    }
    MapDispatchScope scope(vm, get_map_header(data));
    void* ptr = roxy_map_get(const_cast<void*>(data), key_src);
    if (!ptr) {
        *error = "Map key not found";
        return nullptr;
    }
    return static_cast<const u32*>(ptr);
}

void map_insert(RoxyVM* vm, void* data, const u32* key_src, const u32* value_src) {
    MapDispatchScope scope(vm, get_map_header(data));
    roxy_map_insert(data, key_src, value_src);
}

bool map_remove(RoxyVM* vm, void* data, const u32* key_src) {
    MapDispatchScope scope(vm, get_map_header(data));
    return roxy_map_remove(data, key_src);
}

void map_clear(void* data) {
    // `clear` doesn't invoke hash/eq, so no dispatch frame is needed.
    roxy_map_clear(data);
}

void* map_keys(RoxyVM* /*vm*/, void* data) {
    // `roxy_map_keys` walks buckets directly without invoking hash/eq.
    return roxy_map_keys(data);
}

void* map_values(RoxyVM* /*vm*/, void* data) {
    return roxy_map_values(data);
}

}

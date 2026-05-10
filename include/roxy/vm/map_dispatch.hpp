#pragma once

#include "roxy/core/types.hpp"
#include "roxy/rt/roxy_rt.h"

namespace rx {

struct RoxyVM;

// Per-frame state pushed by the VM before each map op so the unified
// `roxy_map_*` runtime — which dispatches custom Hash/Eq through C function
// pointers — can re-enter the interpreter for user-defined `K::hash` /
// `K::eq` impls. The trampoline functions (vm_hash_trampoline /
// vm_eq_trampoline) read the topmost frame, pack args per Roxy's struct ABI,
// and call `call_user_function`.
struct MapDispatchFrame {
    RoxyVM* vm;
    u32 hash_fn_idx;     // UINT32_MAX = no custom hash
    u32 eq_fn_idx;       // UINT32_MAX = no custom eq
    u8  key_slot_count;  // For eq's struct-arg packing (≤2 / ≤4 / ≥5 slots)
};

// Push/pop a dispatch frame around a single map operation. The push site
// must wrap *every* `roxy_map_*` call that may invoke hash/eq (insert,
// contains, get, remove, plus rehash via insert_internal — the rehash
// path is reached through insert).
void map_dispatch_push(MapDispatchFrame frame);
void map_dispatch_pop();

// Returns the trampolines for installation in `MapHeader.hash_fn` /
// `MapHeader.eq_fn` when bytecode dispatch is in use. Both are stable
// `extern "C"` function pointers; passing them across the C ABI is safe.
roxy_map_hash_fn map_dispatch_hash_trampoline();
roxy_map_eq_fn   map_dispatch_eq_trampoline();

// Per-map dispatch info kept in a side-table on `RoxyVM`. The unified
// `MapHeader` no longer carries `hash_fn_index`/`eq_fn_index` fields; they
// live here instead so the AOT runtime never sees them. Sentinel
// `UINT32_MAX` for either index means "no custom dispatch — fall back to
// bytewise hash/memcmp".
struct MapDispatchInfo {
    u32 hash_fn_idx;
    u32 eq_fn_idx;
};

void map_dispatch_register(RoxyVM* vm, void* map_ptr, MapDispatchInfo info);
MapDispatchInfo map_dispatch_lookup(RoxyVM* vm, void* map_ptr);
void map_dispatch_unregister(RoxyVM* vm, void* map_ptr);

} // namespace rx

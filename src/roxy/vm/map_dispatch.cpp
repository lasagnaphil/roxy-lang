#include "roxy/vm/map_dispatch.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/core/vector.hpp"

#include <cstring>

namespace rx {

// Thread-local stack of in-flight map operations. The trampoline reads the
// top frame; nested map ops (e.g. a user `K::hash` allocating its own
// `Map<X, Y>`) push deeper frames and pop on return.
static thread_local Vector<MapDispatchFrame> g_dispatch_stack;

void map_dispatch_push(MapDispatchFrame frame) {
    g_dispatch_stack.push_back(frame);
}

void map_dispatch_pop() {
    g_dispatch_stack.pop_back();
}

extern "C" uint64_t vm_hash_trampoline(const void* key_src) {
    // Top of the dispatch stack tells us which VM and which bytecode
    // function to invoke. Empty stack → fall back to a stable hash so we
    // don't crash; this shouldn't happen in practice because the VM-side
    // map ops always push before calling roxy_map_*.
    if (g_dispatch_stack.empty()) return 0;
    const MapDispatchFrame& f = g_dispatch_stack.back();
    if (f.hash_fn_idx == UINT32_MAX) return 0;
    u64 args[1] = { reinterpret_cast<u64>(key_src) };
    return call_user_function(f.vm, f.hash_fn_idx, args, 1);
}

extern "C" bool vm_eq_trampoline(const void* a_void, const void* b_void) {
    if (g_dispatch_stack.empty()) {
        // No frame — degrade to bytewise compare. Caller controls slot
        // count so we can't size it here; return false defensively.
        return false;
    }
    const MapDispatchFrame& f = g_dispatch_stack.back();
    if (f.eq_fn_idx == UINT32_MAX) return false;

    // Roxy's calling convention for `K::eq(self: ref K, other: K)`:
    //   self   → pointer in 1 register (always)
    //   other  → packed bytes for ≤4-slot keys, pointer for ≥5-slot keys
    // The runtime calls eq_fn with two `const void*` arguments — both
    // pointing at the same kind of byte buffer (`key_slot_count * 4` bytes).
    // We re-pack the second arg per the struct-arg ABI before invoking.
    const u32* a = static_cast<const u32*>(a_void);
    const u32* b = static_cast<const u32*>(b_void);
    u8 ksc = f.key_slot_count;

    u64 args[3];
    args[0] = reinterpret_cast<u64>(a);
    u32 argc;
    if (ksc <= 2) {
        args[1] = 0;
        memcpy(&args[1], b, static_cast<size_t>(ksc) * sizeof(u32));
        argc = 2;
    } else if (ksc <= 4) {
        args[1] = 0;
        args[2] = 0;
        memcpy(&args[1], b, sizeof(u64));
        memcpy(&args[2], b + 2, static_cast<size_t>(ksc - 2) * sizeof(u32));
        argc = 3;
    } else {
        // Large struct: pass pointer (matches Roxy ABI for >4 slot keys).
        args[1] = reinterpret_cast<u64>(b);
        argc = 2;
    }
    return call_user_function(f.vm, f.eq_fn_idx, args, argc) != 0;
}

roxy_map_hash_fn map_dispatch_hash_trampoline() {
    return &vm_hash_trampoline;
}

roxy_map_eq_fn map_dispatch_eq_trampoline() {
    return &vm_eq_trampoline;
}

} // namespace rx

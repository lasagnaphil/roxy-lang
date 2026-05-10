#include "roxy/vm/list.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/rt/roxy_rt.h"

#include <cstring>

namespace rx {

// Global list type ID (registered once at startup)
static u32 g_list_type_id = UINT32_MAX;

u32 register_list_type() {
    if (g_list_type_id == UINT32_MAX) {
        g_list_type_id = register_object_type("list", 0, nullptr);
    }
    return g_list_type_id;
}

u32 get_list_type_id() {
    return g_list_type_id;
}

// VM-side wrappers around the unified `roxy_list_*` runtime. Allocation flows
// through the ctx allocator (slab in VM mode); the bounds-checked `*_slots`
// helpers preserve the VM's nullptr+error contract instead of hitting the
// rt's `assert`.

void* list_alloc(RoxyVM* /*vm*/, u32 capacity, u32 element_slot_count, bool element_is_inline) {
    void* data = roxy_list_alloc(static_cast<int32_t>(element_slot_count),
                                 element_is_inline ? 1 : 0);
    if (!data) return nullptr;
    roxy_list_init(data, static_cast<int32_t>(capacity));
    return data;
}

void* list_copy(RoxyVM* /*vm*/, void* src) {
    return roxy_list_copy(src);
}

// ── Slot-based API ──

u32* list_get_ptr(void* data, i64 index, const char** error) {
    if (data == nullptr) {
        *error = "Null list reference";
        return nullptr;
    }

    ListHeader* header = get_list_header(data);

    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "List index out of bounds";
        return nullptr;
    }

    return static_cast<u32*>(roxy_list_get(data, static_cast<int32_t>(index)));
}

bool list_set_slots(void* data, i64 index, const u32* src, const char** error) {
    if (data == nullptr) {
        *error = "Null list reference";
        return false;
    }

    ListHeader* header = get_list_header(data);

    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "List index out of bounds";
        return false;
    }

    roxy_list_set(data, static_cast<int32_t>(index), src);
    return true;
}

void list_push_slots(void* data, const u32* src) {
    roxy_list_push(data, src);
}

u32* list_pop_ptr(void* data) {
    ListHeader* header = get_list_header(data);
    if (header->length == 0) {
        return nullptr;
    }
    return static_cast<u32*>(roxy_list_pop(data));
}

// ── Value-based wrappers (backward compatibility) ──

bool list_get(void* data, i64 index, Value& out, const char** error) {
    const char* err = nullptr;
    u32* ptr = list_get_ptr(data, index, &err);
    if (!ptr) {
        *error = err;
        return false;
    }
    u64 val = 0;
    ListHeader* header = get_list_header(data);
    memcpy(&val, ptr, sizeof(u32) * header->element_slot_count);
    out = Value::from_u64(val);
    return true;
}

bool list_set(void* data, i64 index, Value value, const char** error) {
    u64 val = value.as_u64();
    return list_set_slots(data, index, reinterpret_cast<const u32*>(&val), error);
}

void list_push(void* data, Value value) {
    u64 val = value.as_u64();
    list_push_slots(data, reinterpret_cast<const u32*>(&val));
}

Value list_pop(void* data) {
    u32* ptr = list_pop_ptr(data);
    if (!ptr) {
        return Value::make_null();
    }
    u64 val = 0;
    ListHeader* header = get_list_header(data);
    memcpy(&val, ptr, sizeof(u32) * header->element_slot_count);
    return Value::from_u64(val);
}

}

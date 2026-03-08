#include "roxy/vm/list.hpp"
#include "roxy/vm/object.hpp"

#include <cstdlib>
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

void* list_alloc(RoxyVM* vm, u32 capacity, u32 element_slot_count, bool element_is_inline) {
    // Data size is just the ListHeader (elements are in separate buffer)
    u32 data_size = sizeof(ListHeader);

    // Allocate using object system
    void* data = object_alloc(vm, g_list_type_id, data_size);
    if (!data) {
        return nullptr;
    }

    // Initialize list header
    ListHeader* header = get_list_header(data);
    header->length = 0;
    header->capacity = capacity;
    header->element_slot_count = element_slot_count;
    header->element_is_inline = element_is_inline;

    // Allocate element buffer if capacity > 0
    if (capacity > 0) {
        header->elements = static_cast<u32*>(malloc(sizeof(u32) * element_slot_count * capacity));
        memset(header->elements, 0, sizeof(u32) * element_slot_count * capacity);
    } else {
        header->elements = nullptr;
    }

    return data;
}

void* list_copy(RoxyVM* vm, void* src) {
    if (!src) return nullptr;
    const ListHeader* src_header = get_list_header(src);
    void* dst = list_alloc(vm, src_header->capacity, src_header->element_slot_count, src_header->element_is_inline);
    if (!dst) return nullptr;
    ListHeader* dst_header = get_list_header(dst);
    dst_header->length = src_header->length;
    if (src_header->length > 0) {
        memcpy(dst_header->elements, src_header->elements,
               sizeof(u32) * src_header->element_slot_count * src_header->length);
    }
    return dst;
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

    return list_element_ptr(header, static_cast<u32>(index));
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

    memcpy(list_element_ptr(header, static_cast<u32>(index)), src,
           sizeof(u32) * header->element_slot_count);
    return true;
}

void list_push_slots(void* data, const u32* src) {
    ListHeader* header = get_list_header(data);
    u32 esc = header->element_slot_count;

    // Grow if needed
    if (header->length >= header->capacity) {
        u32 new_cap = header->capacity == 0 ? 8 : header->capacity * 2;
        u32* new_elements = static_cast<u32*>(malloc(sizeof(u32) * esc * new_cap));
        if (header->elements) {
            memcpy(new_elements, header->elements, sizeof(u32) * esc * header->length);
            free(header->elements);
        }
        header->elements = new_elements;
        header->capacity = new_cap;
    }

    memcpy(list_element_ptr(header, header->length), src, sizeof(u32) * esc);
    header->length++;
}

u32* list_pop_ptr(void* data) {
    ListHeader* header = get_list_header(data);

    if (header->length == 0) {
        return nullptr;
    }

    header->length--;
    return list_element_ptr(header, header->length);
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

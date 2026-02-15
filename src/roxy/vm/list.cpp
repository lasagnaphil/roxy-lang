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

void* list_alloc(RoxyVM* vm, u32 capacity) {
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

    // Allocate element buffer if capacity > 0
    if (capacity > 0) {
        header->elements = static_cast<Value*>(malloc(sizeof(Value) * capacity));
        memset(header->elements, 0, sizeof(Value) * capacity);
    } else {
        header->elements = nullptr;
    }

    return data;
}

bool list_get(void* data, i64 index, Value& out, const char** error) {
    if (data == nullptr) {
        *error = "Null list reference";
        return false;
    }

    const ListHeader* header = get_list_header(data);

    // Bounds check
    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "List index out of bounds";
        return false;
    }

    out = header->elements[index];
    return true;
}

bool list_set(void* data, i64 index, Value value, const char** error) {
    if (data == nullptr) {
        *error = "Null list reference";
        return false;
    }

    ListHeader* header = get_list_header(data);

    // Bounds check
    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "List index out of bounds";
        return false;
    }

    header->elements[index] = value;
    return true;
}

void list_push(void* data, Value value) {
    ListHeader* header = get_list_header(data);

    // Grow if needed
    if (header->length >= header->capacity) {
        u32 new_cap = header->capacity == 0 ? 8 : header->capacity * 2;
        Value* new_elements = static_cast<Value*>(malloc(sizeof(Value) * new_cap));
        if (header->elements) {
            memcpy(new_elements, header->elements, sizeof(Value) * header->length);
            free(header->elements);
        }
        header->elements = new_elements;
        header->capacity = new_cap;
    }

    header->elements[header->length] = value;
    header->length++;
}

Value list_pop(void* data) {
    ListHeader* header = get_list_header(data);

    if (header->length == 0) {
        return Value::make_null();
    }

    header->length--;
    return header->elements[header->length];
}

}

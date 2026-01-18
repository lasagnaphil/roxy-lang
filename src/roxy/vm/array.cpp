#include "roxy/vm/array.hpp"
#include "roxy/vm/object.hpp"

namespace rx {

// Global array type ID (registered once at startup)
static u32 g_array_type_id = UINT32_MAX;

u32 register_array_type() {
    if (g_array_type_id == UINT32_MAX) {
        g_array_type_id = register_object_type("array", 0, nullptr);
    }
    return g_array_type_id;
}

u32 get_array_type_id() {
    return g_array_type_id;
}

void* array_alloc(RoxyVM* vm, u32 length) {
    // Ensure array type is registered
    if (g_array_type_id == UINT32_MAX) {
        register_array_type();
    }

    // Calculate total data size: ArrayHeader + (Value * length)
    u32 data_size = sizeof(ArrayHeader) + sizeof(Value) * length;

    // Allocate using object system
    void* data = object_alloc(vm, g_array_type_id, data_size);
    if (!data) {
        return nullptr;
    }

    // Initialize array header
    ArrayHeader* header = get_array_header(data);
    header->length = length;
    header->capacity = length;

    // Initialize all elements to null
    Value* elements = array_elements(data);
    for (u32 i = 0; i < length; i++) {
        elements[i] = Value::make_null();
    }

    return data;
}

bool array_get(void* data, i64 index, Value& out, const char** error) {
    if (data == nullptr) {
        *error = "Null array reference";
        return false;
    }

    const ArrayHeader* header = get_array_header(data);

    // Bounds check
    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "Array index out of bounds";
        return false;
    }

    const Value* elements = array_elements(data);
    out = elements[index];
    return true;
}

bool array_set(void* data, i64 index, Value value, const char** error) {
    if (data == nullptr) {
        *error = "Null array reference";
        return false;
    }

    ArrayHeader* header = get_array_header(data);

    // Bounds check
    if (index < 0 || static_cast<u64>(index) >= header->length) {
        *error = "Array index out of bounds";
        return false;
    }

    Value* elements = array_elements(data);
    elements[index] = value;
    return true;
}

}

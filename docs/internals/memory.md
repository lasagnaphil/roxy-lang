# Memory Model

Roxy uses a reference-counted memory model with three reference types and no garbage collector.

## Reference Types

| Type | Owns? | Nullable? | On dangling |
|------|-------|-----------|-------------|
| `uniq` | Yes | No | N/A (is owner) |
| `ref` | No | No | Assert/crash |
| `weak` | No | Yes | Returns null or asserts |

### Critical Rule: No `ref` in Fields

To prevent reference cycles, `ref` can only be used for:
- Function parameters
- Local variables

Struct fields must use `uniq` (ownership) or `weak` (back-references).

## Object Header

Every heap-allocated object has a header:

```cpp
struct ObjectHeader {
    u32 ref_count;          // Number of active 'ref' pointers
    u32 weak_generation;    // Bumped on delete to invalidate weak refs
    u32 type_id;            // Type identifier for runtime type info
    u32 size;               // Total size including header

    void* data();           // Get pointer to object data (after header)
};
```

## Reference Counting Operations

```cpp
// Reference counting operations
void ref_inc(void* data);
bool ref_dec(RoxyVM* vm, void* data);  // Returns true if deallocated

// Weak reference operations
u32 weak_ref_create(void* data);
bool weak_ref_valid(void* data, u32 generation);
void weak_ref_invalidate(void* data);

// Object allocation/deallocation
void* object_alloc(RoxyVM* vm, u32 type_id, u32 data_size);
void object_free(RoxyVM* vm, void* data);
```

### Implementation Details

```cpp
inline void ref_inc(void* obj) {
    get_header(obj)->ref_count++;
}

inline void ref_dec(void* obj) {
    ObjectHeader* header = get_header(obj);
    assert(header->ref_count > 0);
    header->ref_count--;
}

inline bool weak_is_valid(void* ptr, uint32_t generation) {
    if (!ptr) return false;
    return get_header(ptr)->weak_generation == generation;
}

void dealloc(void* obj) {
    ObjectHeader* header = get_header(obj);
    
    if (header->ref_count > 0) {
        panic("Cannot delete object with active refs");
    }
    
    header->weak_generation++;  // Invalidate weak refs
    release_field_refs(obj, header->type_id);
    std::free(header);
}
```

## Type Descriptor

For runtime type information:

```cpp
struct TypeDescriptor {
    const char* name;
    uint32_t size;
    uint32_t alignment;
    std::vector<FieldDescriptor> fields;
    std::vector<MethodDescriptor> methods;
    
    // For ref counting during destruction
    std::vector<uint16_t> ref_field_offsets;
    std::vector<uint16_t> weak_field_offsets;
};

struct FieldDescriptor {
    const char* name;
    FieldType type;       // Int, Float, Bool, Struct, Ref, Weak, Uniq
    uint16_t offset;
    uint16_t size;
    TypeId nested_type;
};
```

## Files

- `include/roxy/vm/object.hpp` - Object header and ref counting declarations
- `src/roxy/vm/object.cpp` - Object allocation and ref counting implementation

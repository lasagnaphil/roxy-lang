# Lists

Roxy supports dynamically-sized lists with bounds checking, using generic syntax `List<T>`.

## List Layout

Lists are stored as objects with a ListHeader. `ListHeader` is the unified `roxy_list_header` from `roxy_rt.h` — both VM and AOT-compiled programs share one definition. Elements are in a separate malloc'd buffer:

```c
typedef struct {
    uint32_t length;
    uint32_t capacity;
    uint32_t element_slot_count;  // u32 slots per element (1, 2, or N for structs)
    uint8_t  element_is_inline;   // 1 = primitive packed in slots; 0 = struct (caller passes ptr)
    uint8_t  _pad[3];
    uint32_t* elements;           // capacity * element_slot_count u32 slots
} roxy_list_header;

// Memory layout: [ObjectHeader][ListHeader]
// Elements buffer: [u32 * capacity * element_slot_count] (separate allocation)
```

The key design choice: elements are stored in a **separate buffer** rather than inline after the header. This allows `push` to realloc the elements buffer without moving the ObjectHeader (which would invalidate all pointers to the list).

## Construction

```roxy
// Empty list (capacity 0)
var lst: List<i32> = List<i32>();

// List with pre-allocated capacity
var lst: List<i32> = List<i32>(10);
```

`List<T>` is registered as a **generic native type** via `NativeRegistry::register_generic_type`. When the compiler encounters `List<i32>`, the semantic analyzer recognizes it as a native generic, creates a monomorphized struct type (`List$i32`), and resolves its methods and constructor from the registry using `instantiate_generic_methods` / `instantiate_generic_constructor`. Since all Roxy Values are 64-bit, a single set of runtime native functions handles all element types uniformly.

## Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `len()` | `() -> i32` | Return number of elements |
| `cap()` | `() -> i32` | Return allocated capacity |
| `push(val)` | `(T) -> void` | Append element (grows if needed) |
| `pop()` | `() -> T` | Remove and return last element |

## Indexing

List indexing (`list[i]` and `list[i] = val`) is handled via native `index` and `index_mut` methods registered through `NativeRegistry::bind_generic_method`. The compiler resolves these through `TypeCache::lookup_method()` and emits `CallNative` IR ops, the same path used by all other list methods (`.len()`, `.push()`, etc.). Both perform null checks and bounds checking, setting `vm->error` on failure.

## Copy and Move Semantics

A `List<T>` is **noncopyable** when `T` is noncopyable (i.e., `T` is `uniq`, a struct with a default destructor, or another noncopyable container). Noncopyable lists use move semantics — the same rules as `uniq` variables and value structs with destructors:

- **Passing to a function** moves ownership; the caller's variable is consumed
- **Initializing a new variable** (`var copy = items`) moves the source
- **Use-after-move** is a compile-time error
- **Struct fields** of noncopyable list type trigger a synthetic destructor on the containing struct

When `T` is copyable (e.g., `i32`, `string`), the list is freely copyable via a shallow `list_copy` in the function prologue.

### Scope-Exit Cleanup (RAII)

When a noncopyable list goes out of scope, the compiler emits a cleanup loop in the IR:

1. Get the list length
2. For each element: load via `List$$index`, destroy via the element type's destructor + `Delete` (for `uniq` elements)
3. Call `List$$delete` to free the element buffer
4. Call `Delete` to free the slab-allocated list header

This follows the same block-argument loop pattern as `gen_for_stmt` in the IR builder.

## Growth Strategy

When pushing beyond capacity, the list doubles its capacity (minimum 8 elements).

## Usage Example

```roxy
fun quicksort(lst: List<i32>, low: i32, high: i32) {
    if (low < high) {
        var pivot: i32 = lst[high];
        var i: i32 = low - 1;
        for (var j: i32 = low; j < high; j = j + 1) {
            if (lst[j] <= pivot) {
                i = i + 1;
                var temp: i32 = lst[i];
                lst[i] = lst[j];
                lst[j] = temp;
            }
        }
        var temp: i32 = lst[i + 1];
        lst[i + 1] = lst[high];
        lst[high] = temp;
        var pi: i32 = i + 1;
        quicksort(lst, low, pi - 1);
        quicksort(lst, pi + 1, high);
    }
}

fun main(): i32 {
    var lst: List<i32> = List<i32>();
    lst.push(5);
    lst.push(2);
    lst.push(8);
    lst.push(1);
    lst.push(9);

    quicksort(lst, 0, lst.len() - 1);

    for (var i: i32 = 0; i < lst.len(); i = i + 1) {
        print(lst[i]);
    }
    return 0;
}
```

## Files

- `include/roxy/rt/roxy_rt.h` - Unified `roxy_list_header` + `roxy_list_*` C API
- `src/roxy/rt/roxy_rt.cpp` - List allocation/push/pop/get/set implementation shared between VM and AOT
- `include/roxy/vm/list.hpp` - `ListHeader` typedef alias of `roxy_list_header`, VM-side declarations
- `src/roxy/vm/list.cpp` - Thin shim around `roxy_list_*` (preserves the VM's nullptr+error contract on bounds violations)
- `include/roxy/vm/natives.hpp` - Built-in native function constants
- `src/roxy/vm/natives.cpp` - Native function implementations (List<T> generic type registration)
- `include/roxy/vm/binding/registry.hpp` - Generic type registration API

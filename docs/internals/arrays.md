# Lists

Roxy supports dynamically-sized lists with bounds checking, using generic syntax `List<T>`.

## List Layout

Lists are stored as objects with a ListHeader. Elements are in a separate malloc'd buffer:

```cpp
struct ListHeader {
    u32 length;    // Number of elements
    u32 capacity;  // Allocated capacity
    Value* elements; // Separate malloc'd buffer (nullptr if capacity == 0)
};

// Memory layout: [ObjectHeader][ListHeader]
// Elements buffer: [Value * capacity] (separate allocation)
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

- `include/roxy/vm/list.hpp` - ListHeader and list operations
- `src/roxy/vm/list.cpp` - List allocation and access implementation
- `include/roxy/vm/natives.hpp` - Built-in native function constants
- `src/roxy/vm/natives.cpp` - Native function implementations (List<T> generic type registration)
- `include/roxy/vm/binding/registry.hpp` - Generic type registration API

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

`List<T>` is a **built-in type**, not a generic struct that goes through monomorphization. Since all Roxy Values are 64-bit, a single runtime implementation handles all element types uniformly. The compiler recognizes "List" as a special name (like "string") and constructs the appropriate type from the type argument.

## Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `len()` | `() -> i32` | Return number of elements |
| `cap()` | `() -> i32` | Return allocated capacity |
| `push(val)` | `(T) -> void` | Append element (grows if needed) |
| `pop()` | `() -> T` | Remove and return last element |

## Indexing Opcodes

| Opcode | Format | Description |
|--------|--------|-------------|
| `GET_INDEX` (0xC0) | ABC | `dst = src1[src2]` - Load list element |
| `SET_INDEX` (0xC1) | ABC | `dst[src1] = src2` - Store list element |

Both opcodes perform null checks and bounds checking, setting `vm->error` on failure.

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
- `include/roxy/vm/natives.hpp` - Native list functions
- `src/roxy/vm/natives.cpp` - Native function implementations

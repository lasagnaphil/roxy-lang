# Arrays

Roxy supports dynamically-sized arrays with bounds checking.

## Array Layout

Arrays are stored as objects with an ArrayHeader followed by Value elements:

```cpp
struct ArrayHeader {
    u32 length;    // Number of elements
    u32 capacity;  // Allocated capacity (for future growth)
};

// Memory layout: [ObjectHeader][ArrayHeader][Value * length]
```

## Array Operations

```cpp
// Allocate an array with given length (capacity = length)
void* array_alloc(RoxyVM* vm, u32 length);

// Get array length
u32 array_length(void* data);

// Bounds-checked element access
bool array_get(void* data, i64 index, Value& out, const char** error);
bool array_set(void* data, i64 index, Value value, const char** error);
```

## Array Opcodes

| Opcode | Format | Description |
|--------|--------|-------------|
| `GET_INDEX` (0xC0) | ABC | `dst = src1[src2]` - Load array element |
| `SET_INDEX` (0xC1) | ABC | `dst[src1] = src2` - Store array element |

Both opcodes perform null checks and bounds checking, setting `vm->error` on failure.

## Built-in Array Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `array_new_int` | `(size: i32) -> i32[]` | Allocate int array initialized to 0 |
| `array_len` | `(arr: i32[]) -> i32` | Return array length |

## Usage Example

```roxy
fun sum_array(): i32 {
    var arr: i32[] = array_new_int(5);
    arr[0] = 1;
    arr[1] = 2;
    arr[2] = 3;
    arr[3] = 4;
    arr[4] = 5;

    var sum: i32 = 0;
    for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
        sum = sum + arr[i];
    }
    return sum;  // Returns 15
}
```

## Files

- `include/roxy/vm/array.hpp` - ArrayHeader and array operations
- `src/roxy/vm/array.cpp` - Array allocation and access implementation
- `include/roxy/vm/natives.hpp` - Native array functions
- `src/roxy/vm/natives.cpp` - Native function implementations

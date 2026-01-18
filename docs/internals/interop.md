# Native Functions and C++ Interop

Roxy provides type-safe C++ function binding with automatic wrapper generation.

## Native Function Infrastructure

Native functions are registered with the bytecode module and called via `CALL_NATIVE`:

```cpp
// Native function signature
typedef void (*NativeFunction)(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);

// Register a native function
void vm_register_native(BCModule* module, StringView name, NativeFunction func);
```

### Calling Convention

```
CALL_NATIVE dst, func_idx, argc

Arguments:  dst+1, dst+2, ... (consecutive registers)
Return:     dst
```

The native function reads arguments from `vm->call_stack.back().registers[first_arg + i]` and writes the result to `registers[dst]`.

## C++ Interop System

The interop system in `include/roxy/vm/binding/` provides automatic wrapper generation.

### Core Components

| File | Purpose |
|------|---------|
| `type_traits.hpp` | `RoxyType<T>` - Maps C++ types to Roxy types |
| `function_traits.hpp` | `FunctionTraits` - Compile-time signature extraction |
| `binder.hpp` | `FunctionBinder` - Automatic wrapper generation |
| `registry.hpp` | `NativeRegistry` - Unified registration system |

### Type Traits

```cpp
// Maps C++ types to Roxy types at compile time
template<> struct RoxyType<i32> {
    static Type* get(TypeCache& tc) { return tc.i32_type(); }
    static i32 from_reg(u64 reg) { return static_cast<i32>(reg); }
    static u64 to_reg(i32 val) { return static_cast<u64>(val); }
};
// Specializations for: void, bool, i8-i64, u8-u64, f32, f64
```

### Function Binder

```cpp
// Automatically generates native wrapper for any C++ function
template<auto FnPtr>
struct FunctionBinder {
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        u64* regs = vm->call_stack.back().registers;
        // Extract args from registers, call function, store result
        auto result = call_with_args(FnPtr, regs, first_arg);
        regs[dst] = RoxyType<decltype(result)>::to_reg(result);
    }
    static NativeFunction get() { return &invoke; }
};
```

### NativeRegistry

```cpp
class NativeRegistry {
public:
    // Automatic binding - generates wrapper from C++ function
    template<auto FnPtr>
    void bind(const char* name);

    // Manual binding - for functions needing VM access (arrays, etc.)
    void bind_native(const char* name, NativeFunction func,
                     std::initializer_list<NativeTypeKind> params,
                     NativeTypeKind return_type);

    // Apply to compiler (semantic analysis)
    void apply_to_symbols(SymbolTable& symbols);

    // Apply to runtime (VM execution)
    void apply_to_module(BCModule* module);

    // Lookup for IR builder
    i32 get_index(StringView name) const;
    bool is_native(StringView name) const;
};
```

## Usage Example

```cpp
// Simple C++ functions
i32 my_add(i32 a, i32 b) { return a + b; }
f64 my_sqrt(f64 x) { return std::sqrt(x); }

// Setup
BumpAllocator allocator(8192);
TypeCache types(allocator);
NativeRegistry registry(allocator, types);

// Automatic binding - wrapper generated at compile time
registry.bind<my_add>("add");
registry.bind<my_sqrt>("sqrt");

// Register built-in natives (array_new_int, array_len, print)
register_builtin_natives(registry);

// Use in compilation
SemanticAnalyzer analyzer(allocator, &registry);
IRBuilder ir_builder(allocator, types, &registry);

// Apply to bytecode module for runtime
registry.apply_to_module(module);
```

## NativeTypeKind

For functions needing VM access (like array operations), use `bind_native` with type kinds:

```cpp
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    ArrayI32,  // i32[]
};

// Example: array_new_int(size: i32) -> i32[]
registry.bind_native("array_new_int", native_array_new_int,
                     {NativeTypeKind::I32}, NativeTypeKind::ArrayI32);
```

## Built-in Native Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `array_new_int` | `(size: i32) -> i32[]` | Allocate int array initialized to 0 |
| `array_len` | `(arr: i32[]) -> i32` | Return array length |
| `print` | `(value: i32) -> void` | Print value to stdout |

## Semantic Integration

Built-in functions are registered during semantic analysis initialization. The IR builder detects calls to these functions and emits `CallNative` IR instead of regular `Call` IR.

## Files

- `include/roxy/vm/binding/type_traits.hpp` - RoxyType mappings
- `include/roxy/vm/binding/function_traits.hpp` - Compile-time signature extraction
- `include/roxy/vm/binding/binder.hpp` - Automatic wrapper generation
- `include/roxy/vm/binding/registry.hpp` - NativeRegistry class
- `include/roxy/vm/binding/interop.hpp` - Convenience header
- `include/roxy/vm/natives.hpp` - Built-in native functions
- `src/roxy/vm/natives.cpp` - Native function implementations

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
// Automatically generates native wrapper for any C++ function.
// The C++ function must take RoxyVM* as its first parameter.
template<auto FnPtr>
struct FunctionBinder {
    static void invoke(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg) {
        u64* regs = vm->call_stack.back().registers;
        // Pass vm as first arg, extract remaining args from registers, store result
        auto result = FnPtr(vm, RoxyType<Arg1>::from_reg(regs[first_arg]), ...);
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

    // Manual binding - for functions needing VM access
    void bind_native(const char* name, NativeFunction func,
                     std::initializer_list<NativeTypeKind> params,
                     NativeTypeKind return_type);

    // Struct registration and method binding
    void register_struct(const char* name, std::initializer_list<NativeFieldEntry> fields);
    template<auto FnPtr>
    void bind_method(const char* struct_name, const char* method_name);
    void bind_method_native(const char* struct_name, const char* method_name,
                            NativeFunction func, ...);

    // Generic native type registration (e.g., List<T>)
    void register_generic_type(const char* name, u32 type_param_count,
                               const char* alloc_name, NativeFunction alloc_func);
    void bind_generic_method(const char* type_name, const char* method_name,
                             NativeFunction func,
                             std::initializer_list<NativeParamDesc> params,
                             NativeParamDesc return_desc);
    void bind_generic_constructor(const char* type_name, NativeFunction func,
                                  u32 min_args,
                                  std::initializer_list<NativeParamDesc> params);
    void bind_generic_destructor(const char* type_name, NativeFunction func);

    // Apply to compiler (semantic analysis)
    void apply_structs_to_types(TypeCache& types, BumpAllocator& alloc, SymbolTable& syms);
    void apply_methods_to_types(TypeCache& types, BumpAllocator& alloc);
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
// Simple C++ functions (all take RoxyVM* as first parameter)
i32 my_add(RoxyVM* vm, i32 a, i32 b) { return a + b; }
f64 my_sqrt(RoxyVM* vm, f64 x) { return std::sqrt(x); }

// Setup
BumpAllocator allocator(8192);
TypeCache types(allocator);
NativeRegistry registry(allocator, types);

// Automatic binding - wrapper generated at compile time
registry.bind<my_add>("add");
registry.bind<my_sqrt>("sqrt");

// Register built-in natives (list_new, list_len, list_push, list_pop, print, etc.)
register_builtin_natives(registry);

// Use in compilation - pass shared TypeCache for type consistency
ModuleRegistry modules(allocator);
modules.register_native_module("builtin", &registry, types);
SemanticAnalyzer analyzer(allocator, types);
analyzer.set_module_registry(&modules);
IRBuilder ir_builder(allocator, types, registry, analyzer.symbols(), modules);

// Apply to bytecode module for runtime
registry.apply_to_module(module);
```

## NativeTypeKind

For functions needing VM access (like list operations), use `bind_native` with type kinds:

```cpp
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,    // string
};

// Example: list_new(cap?: i64) -> List<T>
registry.bind_native("list_new", native_list_new,
                     {NativeTypeKind::I64}, NativeTypeKind::I64);
```

## Built-in Native Functions

### List Methods (Generic Native Type)

`List<T>` is registered as a generic native type via `register_generic_type`. Its methods use `NativeParamDesc` with `type_param(0)` for the element type `T`, allowing monomorphized type resolution at instantiation time.

| Registration | Native Name | Roxy Signature | Description |
|--------------|-------------|----------------|-------------|
| `register_generic_type` | `list_alloc` | (internal) | Allocate empty list object |
| `bind_generic_constructor` | `List$$new` | `List<T>(cap?: i32)` | Initialize with optional capacity |
| `bind_generic_destructor` | `List$$delete` | (auto) | Free element buffer |
| `bind_generic_method` | `List$$len` | `.len() -> i32` | Return list length |
| `bind_generic_method` | `List$$cap` | `.cap() -> i32` | Return list capacity |
| `bind_generic_method` | `List$$push` | `.push(val: T) -> void` | Append element |
| `bind_generic_method` | `List$$pop` | `.pop() -> T` | Remove and return last element |

### String Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `str_concat` | `(a: string, b: string) -> string` | Concatenate two strings |
| `str_eq` | `(a: string, b: string) -> bool` | Test string equality |
| `str_ne` | `(a: string, b: string) -> bool` | Test string inequality |
| `str_len` | `(s: string) -> i32` | Return string length |
| `print_str` | `(s: string) -> void` | Print string to stdout |

### Other Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `print` | `(value: i32) -> void` | Print integer to stdout |

## Native Struct and Method Binding

Game engine embedders can expose C++-defined structs and their methods to Roxy scripts.

### Registering a Native Struct

```cpp
registry.register_struct("Point", {
    {"x", NativeTypeKind::I32},
    {"y", NativeTypeKind::I32}
});
```

This creates a Roxy struct type with the given fields. Native structs behave like regular Roxy structs — they support field access and struct literal initialization:

```roxy
var p = Point { x = 3, y = 4 };
var sum = p.x + p.y;  // field access works
```

Native structs have `decl = nullptr` (no AST node) since they are defined from C++, not parsed from source.

### Auto-Binding Methods

Write a free C++ function where the first parameter is `RoxyVM*` and the second is a pointer to the struct:

```cpp
struct CppPoint { i32 x, y; };

i32 point_sum(RoxyVM* vm, CppPoint* self) { return self->x + self->y; }
i32 point_scaled(RoxyVM* vm, CppPoint* self, i32 scale) { return (self->x + self->y) * scale; }
bool point_is_origin(RoxyVM* vm, CppPoint* self) { return self->x == 0 && self->y == 0; }
```

Then bind with `bind_method<FnPtr>`:

```cpp
registry.bind_method<point_sum>("Point", "sum");
registry.bind_method<point_scaled>("Point", "scaled");
registry.bind_method<point_is_origin>("Point", "is_origin");
```

The `RoxyVM*` parameter is provided automatically by the binding system. The `RoxyType<T*>` specialization handles pointer extraction from registers automatically. Both `RoxyVM*` and the self parameter are excluded from the Roxy-visible parameter count — `point_scaled` appears as a 1-parameter method in Roxy:

```roxy
fun test(): i32 {
    var p = Point { x = 3, y = 4 };
    return p.scaled(10);  // calls point_scaled(&p, 10)
}
```

### Manual Method Binding

For methods that need direct VM access (e.g., for allocation or complex register manipulation):

```cpp
// With type kinds (portable across TypeCache instances)
registry.bind_method_native("Point", "product", native_product_fn,
                            {NativeTypeKind::I32},  // extra params after self
                            NativeTypeKind::I32);   // return type

// With Type* (must use same TypeCache as compilation)
registry.bind_method_manual("Point", "distance", native_distance_fn,
                            {types.f64_type()},     // extra params after self
                            types.f64_type());      // return type
```

Manual method wrappers receive self as `regs[first_arg]` (a pointer to the struct on the stack), followed by any additional arguments.

### Name Mangling

Method entries are stored with mangled names using the `$$` separator: `Point$$sum`, `Point$$scaled`, etc. This is the same convention used by the IR builder for Roxy-defined methods. The mangled name is used as the registry key, so existing `get_index()`, `apply_to_module()`, and `is_native()` work without changes.

### Compilation Pipeline Integration

Native structs and methods are integrated into the semantic analysis passes:

| Pass | Action |
|------|--------|
| 0c | `apply_to_symbols()` — registers non-method native functions in the symbol table |
| 1 | Collect user-defined type declarations |
| 1.5 | `apply_structs_to_types()` — creates struct types, registers in TypeCache and SymbolTable |
| 1.6 | `apply_methods_to_types()` — looks up struct types, attaches `MethodInfo` entries |
| 2 | Resolve type members (user-defined structs) |
| 3 | Analyze function bodies (method calls resolved via normal type hierarchy lookup) |

The `SemanticAnalyzer` accepts an optional `NativeRegistry*`:

```cpp
SemanticAnalyzer analyzer(allocator, types, modules, &registry);
```

During IR generation, the builder checks the native registry for mangled method names. If found, it emits `CallNative` instead of `Call`:

```
// IR for p.sum() where sum is a native method:
v3 = call_native "Point$$sum" [v2]   // v2 = pointer to p
```

### Complete Example

```cpp
// C++ side
struct CppPoint { i32 x, y; };
i32 point_sum(RoxyVM* vm, CppPoint* self) { return self->x + self->y; }

BumpAllocator allocator(8192);
TypeCache types(allocator);
NativeRegistry registry(allocator, types);

// Register struct and method
registry.register_struct("Point", {
    {"x", NativeTypeKind::I32},
    {"y", NativeTypeKind::I32}
});
registry.bind_method<point_sum>("Point", "sum");

// Compile and run
ModuleRegistry modules(allocator);
SemanticAnalyzer analyzer(allocator, types, modules, &registry);
analyzer.analyze(program);

IRBuilder ir_builder(allocator, types, registry, analyzer.symbols(), modules);
IRModule* ir_module = ir_builder.build(program);

BytecodeBuilder bc_builder;
BCModule* module = bc_builder.build(ir_module);
registry.apply_to_module(module);

// Execute
RoxyVM vm;
vm_init(&vm);
vm_load_module(&vm, module);
vm_call(&vm, "test", {});
```

```roxy
// Roxy side
fun test(): i32 {
    var p = Point { x = 3, y = 4 };
    return p.sum();  // returns 7
}
```

### Memory Layout Compatibility

The C++ struct and the Roxy native struct must have matching field layout. Roxy uses slot-based layout (1 slot = 4 bytes for 32-bit types, 2 slots for 64-bit types), and fields are laid out sequentially in declaration order with no padding. The C++ struct passed to method functions should match this layout.

For the `Point` example above:
- Roxy layout: `[x: i32 (slot 0), y: i32 (slot 1)]` = 8 bytes
- C++ `struct CppPoint { i32 x, y; }` = 8 bytes (matching)

## Generic Native Types

The `NativeRegistry` supports registering generic native types (e.g., `List<T>`) that participate in the compiler's monomorphization pipeline. Unlike user-defined generic structs (which use AST cloning), generic native types use `NativeParamDesc` to describe parameter types that reference type parameters.

### NativeParamDesc

```cpp
struct NativeParamDesc {
    bool is_type_param;
    NativeTypeKind kind;          // when !is_type_param (concrete type)
    u32 type_param_index;         // when is_type_param (0=T, 1=U, ...)
};

// Helpers
NativeParamDesc concrete_param(NativeTypeKind k);  // e.g., concrete_param(TK::I32)
NativeParamDesc type_param(u32 index);              // e.g., type_param(0) for T
```

### Registration Example

```cpp
using TK = NativeTypeKind;

// Register List<T> with 1 type parameter and its allocator
registry.register_generic_type("List", 1, "list_alloc", native_list_alloc);

// Constructor: List<T>(cap?: i32)  (min_args=0, so cap is optional)
registry.bind_generic_constructor("List", native_list_init,
                                  0, {concrete_param(TK::I32)});

// Destructor
registry.bind_generic_destructor("List", native_list_delete);

// Methods — type_param(0) resolves to the concrete T at instantiation
registry.bind_generic_method("List", "push", native_list_push,
                             {type_param(0)}, concrete_param(TK::Void));
registry.bind_generic_method("List", "pop",  native_list_pop,
                             {}, type_param(0));
```

### Integration with Monomorphization

When the semantic analyzer encounters `List<i32>`, it:
1. Checks `NativeRegistry::has_generic_type("List")` — finds it
2. Calls `instantiate_generic_methods("List", {i32_type}, ...)` to get concrete `MethodInfo` entries (e.g., `push(val: i32) -> void`)
3. Calls `instantiate_generic_constructor("List", {i32_type}, ...)` to get the constructor signature
4. Attaches the resolved methods/constructor to the monomorphized struct type (`List$i32`)

The runtime native functions are type-erased (all Roxy Values are 64-bit), so a single implementation handles all element types.

## Semantic Integration

Built-in functions are registered during semantic analysis initialization. The IR builder detects calls to these functions and emits `CallNative` IR instead of regular `Call` IR.

## RoxyList<T> — List Interop

`RoxyList<T>` is a thin non-owning typed wrapper around a Roxy list data pointer. It allows C++ bound functions to read, modify, and create lists.

### Usage in Bound Functions

```cpp
// C++ function that reads a list created by Roxy
i32 list_sum(RoxyVM* vm, RoxyList<i32> list) {
    i32 total = 0;
    for (u32 i = 0; i < list.len(); i++) {
        total += list.get(static_cast<i64>(i));
    }
    return total;
}

// C++ function that modifies a list
void list_push_42(RoxyVM* vm, RoxyList<i32> list) {
    list.push(42);
}

// Register
registry.bind<list_sum>("list_sum");
registry.bind<list_push_42>("list_push_42");
```

### API

| Method | Signature | Description |
|--------|-----------|-------------|
| `alloc` | `static RoxyList<T> alloc(RoxyVM*, u32 cap)` | Factory: allocate a new list |
| `get` | `T get(i64 index) const` | Bounds-checked element access |
| `set` | `void set(i64 index, T value)` | Bounds-checked element write |
| `push` | `void push(T value)` | Append element (grows if needed) |
| `pop` | `T pop()` | Remove and return last element |
| `len` | `u32 len() const` | Current length |
| `cap` | `u32 cap() const` | Current capacity |
| `is_valid` | `bool is_valid() const` | Check for null data pointer |
| `data` | `void* data() const` | Raw list data pointer |

### RoxyType Specialization

The `RoxyType<RoxyList<T>>` specialization enables automatic type resolution:
- `get(tc)` returns `tc.list_type(RoxyType<T>::get(tc))` — resolves to `List<i32>`, `List<f64>`, etc.
- `from_reg(u64)` / `to_reg(RoxyList<T>)` handle register ↔ wrapper conversion

## RoxyString — String Interop

`RoxyString` is a thin non-owning wrapper around a Roxy string data pointer. It allows C++ bound functions to read, create, compare, and concatenate Roxy strings.

### Usage in Bound Functions

```cpp
// C++ function that reads a string from Roxy
i32 str_get_len(RoxyVM* vm, RoxyString str) {
    return static_cast<i32>(str.length());
}

// C++ function that creates a string for Roxy
RoxyString str_make_greeting(RoxyVM* vm) {
    return RoxyString::alloc(vm, "hello from C++");
}

// C++ function that concatenates two strings
RoxyString str_join(RoxyVM* vm, RoxyString a, RoxyString b) {
    return a.concat(vm, b);
}

// Register
registry.bind<str_get_len>("str_get_len");
registry.bind<str_make_greeting>("str_make_greeting");
registry.bind<str_join>("str_join");
```

### API

| Method | Signature | Description |
|--------|-----------|-------------|
| `alloc` | `static RoxyString alloc(RoxyVM*, const char*, u32 length)` | Factory: allocate a new string |
| `alloc` | `static RoxyString alloc(RoxyVM*, const char*)` | Factory: allocate (uses strlen) |
| `length` | `u32 length() const` | String length (excluding null terminator) |
| `c_str` | `const char* c_str() const` | Null-terminated character data |
| `equals` | `bool equals(const RoxyString&) const` | Equality comparison |
| `concat` | `RoxyString concat(RoxyVM*, RoxyString) const` | Concatenate, returns new string |
| `is_valid` | `bool is_valid() const` | Check for null data pointer |
| `data` | `void* data() const` | Raw string data pointer |

### RoxyType Specialization

The `RoxyType<RoxyString>` specialization enables automatic type resolution:
- `get(tc)` returns `tc.string_type()`
- `from_reg(u64)` / `to_reg(RoxyString)` handle register ↔ wrapper conversion

## Roxy-Native C++ Containers

> **Note:** This feature is planned but not yet implemented.

Roxy provides its own C++ container library designed for zero-cost interop with the VM. These containers share the exact same memory layout as Roxy's runtime representations, eliminating conversion overhead.

### Design Philosophy

Roxy provides native C++ containers that:

- Have identical memory layout to Roxy runtime containers
- Can be passed to/from Roxy without conversion
- Use the Roxy VM allocator for memory management
- Provide C++ compile-time type safety

### Container Types

```cpp
namespace rx {

template<typename T>
class Array {
    ListHeader* m_header;
public:
    // Element access
    T& operator[](u32 i) { return data()[i]; }
    const T& operator[](u32 i) const { return data()[i]; }
    T* data() { return reinterpret_cast<T*>(m_header + 1); }

    // Size
    u32 length() const { return m_header->length; }
    u32 capacity() const { return m_header->capacity; }
    bool empty() const { return length() == 0; }

    // Iteration (range-for compatible)
    T* begin() { return data(); }
    T* end() { return data() + length(); }
    const T* begin() const { return data(); }
    const T* end() const { return data() + length(); }

    // Modification (requires VM for allocation)
    void push(VM* vm, T value);
    T pop();
    void clear();
};

template<typename K, typename V>
class Dict {
    DictHeader* m_header;
public:
    V* get(const K& key);
    const V* get(const K& key) const;
    void set(VM* vm, K key, V value);
    bool contains(const K& key) const;
    void remove(const K& key);
    u32 size() const;

    // Iteration
    struct Iterator { /* ... */ };
    Iterator begin();
    Iterator end();
};

template<typename T>
class Option {
    bool m_has_value;
    T m_value;
public:
    bool has_value() const { return m_has_value; }
    T& value() { return m_value; }
    const T& value() const { return m_value; }
    T value_or(T default_val) const;

    static Option some(T v) { return Option{true, v}; }
    static Option none() { return Option{false, T{}}; }
};

template<typename T, typename E>
class Result {
    bool m_is_ok;
    union { T m_value; E m_error; };
public:
    bool is_ok() const { return m_is_ok; }
    bool is_err() const { return !m_is_ok; }
    T& value() { return m_value; }
    E& error() { return m_error; }

    static Result ok(T v);
    static Result err(E e);
};

template<typename T, typename U>
class Pair {
public:
    T first;
    U second;

    Pair(T f, U s) : first(f), second(s) {}
};

} // namespace rx
```

### Zero-Cost Interop

Since Roxy containers have the same layout on both sides, no conversion is needed:

```cpp
// C++ function operating directly on Roxy array
void damage_all_enemies(rx::Array<Enemy>& enemies, i32 damage) {
    for (Enemy& e : enemies) {
        e.hp -= damage;
    }
}

// Lookup in Roxy dict
rx::Option<Item> find_in_inventory(rx::Dict<rx::String, Item>& inv, rx::String name) {
    if (Item* item = inv.get(name)) {
        return rx::Option<Item>::some(*item);
    }
    return rx::Option<Item>::none();
}

// Bind directly - array/dict pass through unchanged
registry.bind<damage_all_enemies>("damage_all_enemies");
registry.bind<find_in_inventory>("find_in_inventory");
```

### RoxyType Specializations

The binding system recognizes Roxy containers automatically:

```cpp
template<typename T>
struct RoxyType<rx::Array<T>> {
    static Type* get(TypeCache& tc) {
        return tc.list_type(RoxyType<T>::get(tc));
    }
    static rx::Array<T> from_reg(u64 reg) {
        return rx::Array<T>::from_ptr(reinterpret_cast<void*>(reg));
    }
    static u64 to_reg(rx::Array<T> arr) {
        return reinterpret_cast<u64>(arr.header());
    }
};

template<typename T>
struct RoxyType<rx::Option<T>> {
    static Type* get(TypeCache& tc) {
        return tc.option_type(RoxyType<T>::get(tc));
    }
    // ...
};
```

### File Structure (Planned)

```
include/roxy/
├── containers/
│   ├── list.hpp        # rx::List<T>
│   ├── dict.hpp        # rx::Dict<K,V>
│   ├── option.hpp      # rx::Option<T>
│   ├── result.hpp      # rx::Result<T,E>
│   ├── pair.hpp        # rx::Pair<T,U>
│   └── containers.hpp  # Convenience include-all
```

## Files

- `include/roxy/vm/binding/type_traits.hpp` - RoxyType mappings
- `include/roxy/vm/binding/function_traits.hpp` - Compile-time signature extraction
- `include/roxy/vm/binding/binder.hpp` - Automatic wrapper generation
- `include/roxy/vm/binding/registry.hpp` - NativeRegistry class (declarations + templates)
- `src/roxy/vm/binding/registry.cpp` - NativeRegistry non-template method implementations
- `include/roxy/vm/binding/roxy_string.hpp` - RoxyString wrapper + RoxyType specialization
- `include/roxy/vm/binding/interop.hpp` - Convenience header
- `include/roxy/vm/natives.hpp` - Built-in native functions
- `src/roxy/vm/natives.cpp` - Native function implementations (incl. List<T> generic type registration)
- `include/roxy/containers/` - Roxy-native C++ containers (planned)

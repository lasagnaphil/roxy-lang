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

    // String-based binding - uses Roxy signature strings
    void bind_native(NativeFunction func, const char* signature);
    void bind_native(const char* override_name, NativeFunction func, const char* signature);

    // Struct registration
    void register_struct(const char* name, std::initializer_list<NativeFieldEntry> fields);

    // Auto-bind method (C++ function with wrapper generation)
    template<auto FnPtr>
    void bind_method(const char* struct_name, const char* method_name);

    // String-based method binding (concrete or generic types)
    void bind_method(NativeFunction func, const char* signature);
    void bind_constructor(NativeFunction func, const char* signature, u32 min_args = 0);

    // Generic native type registration
    void register_generic_type(const char* type_decl,
                               const char* alloc_name, NativeFunction alloc_func);
    void bind_generic_destructor(const char* type_name, NativeFunction func);
    void bind_generic_copy_constructor(const char* type_name,
                                       const char* copy_func_name, NativeFunction func);

    // Apply to compiler (semantic analysis)
    void apply_structs_to_types(TypeEnv& type_env, BumpAllocator& alloc, SymbolTable& syms);
    void apply_methods_to_types(TypeEnv& type_env, BumpAllocator& alloc);
    void apply_to_symbols(SymbolTable& symbols);

    // Apply to runtime (VM execution)
    void apply_to_module(BCModule* module);

    // Lookup for IR builder
    i32 get_index(StringView name) const;
    bool is_native(StringView name) const;
};
```

## String-Based Binding API

The primary way to register native functions is using Roxy signature strings. The registry parses these using the actual Roxy parser (Lexer + Parser), so they support the full type syntax.

### Free Functions

```cpp
// Simple binding - name extracted from signature
registry.bind_native(native_print, "fun print(s: string)");
registry.bind_native(native_str_concat, "fun str_concat(a: string, b: string): string");

// Name override - for $$-mangled trait method names
registry.bind_native("bool$$to_string", native_bool_to_string,
                     "fun to_string(val: bool): string");
registry.bind_native("i32$$hash", native_i32_hash,
                     "fun hash(val: i32): i64");
```

### Methods on Concrete Types

```cpp
// Method with no parameters
registry.bind_method(native_product, "fun Point.product(): i32");
```

### Methods on Generic Types

```cpp
// Type parameters in the struct name are resolved at instantiation time
registry.bind_method(native_list_push, "fun List<T>.push(val: T)");
registry.bind_method(native_list_pop,  "fun List<T>.pop(): T");
registry.bind_method(native_map_keys,  "fun Map<K, V>.keys(): List<K>");
```

### Constructors

```cpp
// min_args controls optional parameters
registry.bind_constructor(native_list_init, "fun List<T>.new(cap: i32)", 0);
registry.bind_constructor(native_map_init,
    "fun Map<K, V>.new(key_kind: i32, capacity: i32)", 1);
```

### Generic Type Registration

```cpp
// Type declaration string includes type parameter names
registry.register_generic_type("List<T>", "list_alloc", native_list_alloc);
registry.register_generic_type("Map<K, V>", "map_alloc", native_map_alloc);

// Destructors and copy constructors use the base type name
registry.bind_generic_destructor("List", native_list_delete);
registry.bind_generic_copy_constructor("List", "list_copy", native_list_copy);
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

## Built-in Native Functions

### List Methods (Generic Native Type)

`List<T>` is registered as a generic native type via `register_generic_type("List<T>", ...)`. Its methods use Roxy signature strings with type parameter `T`, which gets resolved to concrete types at instantiation time.

| Registration | Signature | Description |
|---|---|---|
| `register_generic_type` | `"List<T>"` | Allocate empty list object |
| `bind_constructor` | `"fun List<T>.new(cap: i32)"` | Initialize with optional capacity |
| `bind_generic_destructor` | `"List"` | Free element buffer |
| `bind_method` | `"fun List<T>.len(): i32"` | Return list length |
| `bind_method` | `"fun List<T>.cap(): i32"` | Return list capacity |
| `bind_method` | `"fun List<T>.push(val: T)"` | Append element |
| `bind_method` | `"fun List<T>.pop(): T"` | Remove and return last element |
| `bind_method` | `"fun List<T>.index(idx: i32): T"` | Get element by index |
| `bind_method` | `"fun List<T>.index_mut(idx: i32, val: T)"` | Set element by index |

### Map Methods (Generic Native Type)

`Map<K, V>` is registered with 2 type parameters.

| Registration | Signature | Description |
|---|---|---|
| `register_generic_type` | `"Map<K, V>"` | Allocate empty map object |
| `bind_constructor` | `"fun Map<K, V>.new(key_kind: i32, capacity: i32)"` | Initialize (min_args=1) |
| `bind_method` | `"fun Map<K, V>.len(): i32"` | Return map length |
| `bind_method` | `"fun Map<K, V>.contains(key: K): bool"` | Check if key exists |
| `bind_method` | `"fun Map<K, V>.get(key: K): V"` | Get value by key |
| `bind_method` | `"fun Map<K, V>.insert(key: K, val: V)"` | Insert key-value pair |
| `bind_method` | `"fun Map<K, V>.remove(key: K): bool"` | Remove by key |
| `bind_method` | `"fun Map<K, V>.clear()"` | Clear all entries |
| `bind_method` | `"fun Map<K, V>.keys(): List<K>"` | Get all keys as list |
| `bind_method` | `"fun Map<K, V>.values(): List<V>"` | Get all values as list |

### String Functions

| Function | Signature | Description |
|----------|-----------|-------------|
| `str_concat` | `(a: string, b: string) -> string` | Concatenate two strings |
| `str_eq` | `(a: string, b: string) -> bool` | Test string equality |
| `str_ne` | `(a: string, b: string) -> bool` | Test string inequality |
| `str_len` | `(s: string) -> i32` | Return string length |
| `print` | `(s: string) -> void` | Print string to stdout |

## Native Struct and Method Binding

Game engine embedders can expose C++-defined structs and their methods to Roxy scripts.

### Registering a Native Struct

```cpp
registry.register_struct("Point", {
    {"x", NativeTypeKind::I32},
    {"y", NativeTypeKind::I32}
});
```

This creates a Roxy struct type with the given fields. Native structs behave like regular Roxy structs -- they support field access and struct literal initialization:

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

The `RoxyVM*` parameter is provided automatically by the binding system. The `RoxyType<T*>` specialization handles pointer extraction from registers automatically. Both `RoxyVM*` and the self parameter are excluded from the Roxy-visible parameter count -- `point_scaled` appears as a 1-parameter method in Roxy:

```roxy
fun test(): i32 {
    var p = Point { x = 3, y = 4 };
    return p.scaled(10);  // calls point_scaled(&p, 10)
}
```

### Manual Method Binding (Signature-Based)

For methods that need direct VM access (e.g., for allocation or complex register manipulation):

```cpp
registry.bind_method(native_product_fn, "fun Point.product(): i32");
```

Manual method wrappers receive self as `regs[first_arg]` (a pointer to the struct on the stack), followed by any additional arguments.

### Name Mangling

Method entries are stored with mangled names using the `$$` separator: `Point$$sum`, `Point$$scaled`, etc. This is the same convention used by the IR builder for Roxy-defined methods. The mangled name is used as the registry key, so existing `get_index()`, `apply_to_module()`, and `is_native()` work without changes.

### Compilation Pipeline Integration

Native structs and methods are integrated into the semantic analysis passes:

| Pass | Action |
|------|--------|
| 0c | `apply_to_symbols()` -- registers non-method native functions in the symbol table |
| 1 | Collect user-defined type declarations |
| 1.5 | `apply_structs_to_types()` -- creates struct types, registers in TypeCache and SymbolTable |
| 1.6 | `apply_methods_to_types()` -- looks up struct types, attaches `MethodInfo` entries |
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

The `NativeRegistry` supports registering generic native types (e.g., `List<T>`) that participate in the compiler's monomorphization pipeline. Unlike user-defined generic structs (which use AST cloning), generic native types use parsed Roxy signature strings to describe parameter types that reference type parameters.

### Registration Example

```cpp
// Register List<T> with its type parameters and allocator
registry.register_generic_type("List<T>", "list_alloc", native_list_alloc);

// Constructor: List<T>(cap?: i32)  (min_args=0, so cap is optional)
registry.bind_constructor(native_list_init, "fun List<T>.new(cap: i32)", 0);

// Destructor
registry.bind_generic_destructor("List", native_list_delete);

// Methods -- type parameter T is resolved to the concrete type at instantiation
registry.bind_method(native_list_push, "fun List<T>.push(val: T)");
registry.bind_method(native_list_pop,  "fun List<T>.pop(): T");
registry.bind_method(native_list_len,  "fun List<T>.len(): i32");
```

### How Signature Parsing Works

When `bind_native`, `bind_method`, or `bind_constructor` is called with a signature string:

1. The registry prepends `"native "` and appends `";"` to the signature
2. The string is fed through the actual Roxy Lexer and Parser
3. The resulting AST (DeclFun or DeclMethod) provides TypeExpr nodes for all parameter/return types
4. These TypeExpr nodes are stored on the NativeFunctionEntry and resolved to concrete Type* later

For generic types, `register_generic_type("List<T>", ...)` parses the type declaration to extract the type name (`"List"`) and type parameter names (`["T"]`). When methods are instantiated, the TypeExpr nodes are resolved with concrete type arguments substituted for the named parameters.

### Integration with Monomorphization

When the semantic analyzer encounters `List<i32>`, it:
1. Checks `NativeRegistry::has_generic_type("List")` -- finds it
2. Calls `instantiate_generic_methods("List", {i32_type}, ...)` to get concrete `MethodInfo` entries (e.g., `push(val: i32) -> void`)
3. Calls `instantiate_generic_constructor("List", {i32_type}, ...)` to get the constructor signature
4. Attaches the resolved methods/constructor to the monomorphized struct type (`List$i32`)

The runtime native functions are type-erased (all Roxy Values are 64-bit), so a single implementation handles all element types.

## Semantic Integration

Built-in functions are registered during semantic analysis initialization. The IR builder detects calls to these functions and emits `CallNative` IR instead of regular `Call` IR.

## RoxyList<T> -- List Interop

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
- `get(tc)` returns `tc.list_type(RoxyType<T>::get(tc))` -- resolves to `List<i32>`, `List<f64>`, etc.
- `from_reg(u64)` / `to_reg(RoxyList<T>)` handle register <-> wrapper conversion

## RoxyString -- String Interop

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
- `from_reg(u64)` / `to_reg(RoxyString)` handle register <-> wrapper conversion

## Files

- `include/roxy/vm/binding/type_traits.hpp` - RoxyType mappings
- `include/roxy/vm/binding/function_traits.hpp` - Compile-time signature extraction
- `include/roxy/vm/binding/binder.hpp` - Automatic wrapper generation
- `include/roxy/vm/binding/registry.hpp` - NativeRegistry class (declarations + templates)
- `src/roxy/vm/binding/registry.cpp` - NativeRegistry non-template method implementations
- `include/roxy/vm/binding/roxy_string.hpp` - RoxyString wrapper + RoxyType specialization
- `include/roxy/vm/binding/interop.hpp` - Convenience header
- `include/roxy/vm/natives.hpp` - Built-in native functions
- `src/roxy/vm/natives.cpp` - Native function implementations (incl. List<T> and Map<K, V> registration)

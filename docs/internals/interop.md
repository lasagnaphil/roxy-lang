# Native Functions and C++ Interop

Roxy binds C++ functions with type-safe, automatically generated wrappers. A single bound C++ function works unchanged in both VM mode (registered as a `NativeFunction`, called by `CALL_NATIVE`) and AOT mode (the C emitter emits a typed direct call). The binding system lives in `include/roxy/vm/binding/`.

## Calling Convention

Native functions are registered with the bytecode module and invoked via `CALL_NATIVE`:

```
CALL_NATIVE dst, func_idx, argc

Arguments:  dst+1, dst+2, ...  (consecutive registers)
Return:     dst
```

The low-level signature carries the VM and register layout:

```cpp
typedef void (*NativeFunction)(RoxyVM* vm, u8 dst, u8 argc, u8 first_arg);
```

A wrapper reads arguments from `vm->call_stack.back().registers[first_arg + i]` and writes the result to `registers[dst]`.

## Bound Functions Take Only Their Logical Args

Embedder-facing C++ functions are plain `Ret(Args...)` — **no `RoxyVM*` prefix**. Functions that need runtime state (allocator, intern table, embedder `user_data`) call `roxy_get_ctx()` directly; the interpreter activates the active VM's context via `roxy::ScopedContext` on every entry, so the context is always reachable.

```cpp
i32 my_add(i32 a, i32 b) { return a + b; }
f64 my_sqrt(f64 x) { return std::sqrt(x); }

registry.bind<my_add>("add");
registry.bind<my_sqrt>("sqrt");
```

`FunctionBinder<FnPtr>` (`binder.hpp`) generates the `NativeFunction` wrapper at compile time: it extracts each argument from registers via `RoxyType<Arg>::from_reg`, calls the user function, and stores the result via `RoxyType<Ret>::to_reg`.

### Type Mapping

`RoxyType<T>` (`type_traits.hpp`) maps a C++ type to its Roxy type and provides register conversions: `get(TypeCache&)` returns the `Type*`, `from_reg(u64)` / `to_reg(T)` convert to and from a 64-bit register value. Specializations exist for `void`, `bool`, `i8`–`i64`, `u8`–`u64`, `f32`, `f64`, pointers (`T*`), and the interop wrappers `RoxyList<T>` / `RoxyString`. `FunctionTraits` (`function_traits.hpp`) extracts the signature at compile time.

### AOT Mode

In AOT mode the C emitter consults the same `NativeRegistry` (via `CEmitterConfig::native_registry`) and emits a typed direct call to the entry's `aot_symbol_name` (defaults to the registered Roxy name; `bind<FnPtr>(roxy_name, aot_symbol)` lets them diverge). The emitter pre-scans the IR and writes `extern Ret name(Args...);` declarations in the source preamble, so the binary links against either an inline-defined header (`CEmitterConfig::native_include_paths`) or a separately compiled `.cpp` translation unit.

## NativeRegistry

`NativeRegistry` (`registry.hpp`) is the unified registration entry point. It offers automatic binding (`bind`, `bind_method`), string-signature binding (`bind_native`, `bind_method`, `bind_constructor`), struct registration (`register_struct`), and generic-type registration (`register_generic_type`, `bind_generic_destructor`, `bind_generic_copy_constructor`). `apply_*` methods push the registrations into semantic analysis (`apply_to_symbols`, `apply_structs_to_types`, `apply_methods_to_types`) and the runtime (`apply_to_module`); `get_index` / `is_native` serve the IR builder.

## String-Based Binding

The primary registration path uses Roxy signature strings. The registry parses each string through the actual Roxy Lexer + Parser (prepending `native ` and appending `;`), so the full type syntax is supported; the resulting `TypeExpr` nodes are stored on the entry and resolved to concrete `Type*` later.

```cpp
// Free function — name extracted from the signature
registry.bind_native(native_str_concat, "fun str_concat(a: string, b: string): string");

// Name override — e.g. $$-mangled trait method names
registry.bind_native("i32$$hash", native_i32_hash, "fun hash(val: i32): i64");

// Method on a concrete type
registry.bind_method(native_product, "fun Point.product(): i32");

// Method on a generic type — type params resolved at instantiation
registry.bind_method(native_list_push, "fun List<T>.push(val: T)");

// Constructor — min_args controls optional parameters
registry.bind_constructor(native_map_init, "fun Map<K, V>.new(key_kind: i32, capacity: i32)", 1);
```

A signature-bound method wrapper receives `self` as `regs[first_arg]` (a pointer to the struct on the stack), followed by any additional arguments. Use signature binding when a method needs direct VM access (allocation, complex register manipulation).

## Native Structs and Methods

Embedders can expose C++-defined structs and their methods to Roxy scripts.

```cpp
registry.register_struct("Point", {
    {"x", NativeTypeKind::I32},
    {"y", NativeTypeKind::I32}
});
```

This creates a Roxy struct type that behaves like a normal struct (field access, struct-literal init) but has `decl = nullptr` (no AST node). The C++ struct must match Roxy's slot-based layout — fields laid out sequentially in declaration order with no padding (1 slot = 4 bytes for 32-bit types, 2 slots for 64-bit types). For `Point`, both `[x (slot 0), y (slot 1)]` and `struct CppPoint { i32 x, y; }` are 8 bytes.

### Auto-Binding Methods

Write a free C++ function whose first parameter is a pointer to the struct, followed by the method's logical arguments, then bind by pointer:

```cpp
struct CppPoint { i32 x, y; };
i32 point_scaled(CppPoint* self, i32 scale) { return (self->x + self->y) * scale; }

registry.bind_method<point_scaled>("Point", "scaled");
```

`RoxyType<T*>` extracts the pointer from registers automatically. The `self` parameter is excluded from the Roxy-visible parameter count, so `point_scaled` appears as a 1-parameter method:

```roxy
fun test(): i32 {
    var p = Point { x = 3, y = 4 };
    return p.scaled(10);   // calls point_scaled(&p, 10)
}
```

Method entries are stored under mangled names using the `$$` separator (`Point$$scaled`) — the same convention the IR builder uses for Roxy-defined methods — so `get_index()`, `apply_to_module()`, and `is_native()` work unchanged. During IR generation the builder checks the registry for the mangled name and emits `CallNative` instead of `Call`.

### Compilation Pipeline Integration

The `SemanticAnalyzer` accepts an optional `NativeRegistry*`. Native structs and methods slot into the analysis passes:

| Pass | Action |
|------|--------|
| 0c | `apply_to_symbols()` — registers non-method native functions in the symbol table |
| 1.5 | `apply_structs_to_types()` — creates struct types in TypeCache and SymbolTable |
| 1.6 | `apply_methods_to_types()` — attaches `MethodInfo` entries to struct types |
| 3 | Function bodies analyzed; method calls resolve via normal type-hierarchy lookup |

## Generic Native Types

`NativeRegistry` registers generic native types (e.g. `List<T>`, `Map<K, V>`) that participate in monomorphization. Unlike user-defined generic structs (AST cloning), generic native types describe their parameters with parsed signature strings.

```cpp
registry.register_generic_type("List<T>", "list_alloc", native_list_alloc);
registry.bind_constructor(native_list_init, "fun List<T>.new(cap: i32)", 0);  // cap optional
registry.bind_generic_destructor("List", native_list_delete);
registry.bind_generic_copy_constructor("List", "list_copy", native_list_copy);
registry.bind_method(native_list_push, "fun List<T>.push(val: T)");
```

`register_generic_type("List<T>", ...)` parses the declaration to extract the base name (`List`) and parameter names (`[T]`). When the analyzer sees `List<i32>`, it finds the registered type, instantiates concrete `MethodInfo` entries and the constructor with `i32` substituted for `T`, and attaches them to the monomorphized struct type (`List$i32`). The runtime native functions are type-erased — all Roxy values are 64-bit — so one implementation handles every element type.

### Built-in List and Map

`List<T>` and `Map<K, V>` are registered this way in `src/roxy/vm/natives.cpp`. Their methods mirror the Roxy API: `List<T>` provides `new(cap)`, `len`, `cap`, `push`, `pop`, `index`/`index_mut`; `Map<K, V>` provides `new(key_kind, capacity)` (min_args=1), `len`, `contains`, `get`, `insert`, `remove`, `clear`, `keys`, `values`. String operations (`str_concat`, `str_eq`, `str_ne`, `str_len`, `print`) are bound as free functions.

## Interop Wrappers

`RoxyList<T>` (`roxy_string.hpp` sibling) and `RoxyString` (`roxy_string.hpp`) are thin non-owning typed wrappers around a Roxy data pointer, letting bound C++ functions read, modify, and create lists/strings. Their `RoxyType` specializations resolve to `List<T>` / `string` and handle register conversion, so they can be used directly as bound-function parameters and return types.

```cpp
i32 list_sum(RoxyList<i32> list) {
    i32 total = 0;
    for (u32 i = 0; i < list.len(); i++) total += list.get(static_cast<i64>(i));
    return total;
}

RoxyString str_join(RoxyString a, RoxyString b) { return a.concat(b); }

registry.bind<list_sum>("list_sum");
registry.bind<str_join>("str_join");
```

### RoxyList<T>

| Method | Description |
|--------|-------------|
| `static RoxyList<T> alloc(i32 cap = 0)` | Allocate a new list (ctx allocator) |
| `T get(i64 index) const` / `void set(i64, T)` | Bounds-checked access / write |
| `void push(T)` / `T pop()` | Append (grows) / remove last |
| `u32 len() const` / `u32 cap() const` | Length / capacity |
| `bool is_valid() const` / `void* data() const` | Null check / raw pointer |

### RoxyString

| Method | Description |
|--------|-------------|
| `static RoxyString alloc(const char*, u32 length)` / `alloc(const char*)` | Allocate (ctx allocator + intern table); second form uses `strlen` |
| `i32 length() const` / `const char* c_str() const` | Length / null-terminated data |
| `bool equals(RoxyString) const` | Equality |
| `RoxyString concat(RoxyString) const` | Concatenate, returns new string |
| `bool is_valid() const` / `void* data() const` | Null check / raw pointer |

## End-to-End Usage

```cpp
BumpAllocator allocator(8192);
TypeCache types(allocator);
NativeRegistry registry(allocator, types);

registry.register_struct("Point", {{"x", NativeTypeKind::I32}, {"y", NativeTypeKind::I32}});
registry.bind_method<point_sum>("Point", "sum");
register_builtin_natives(registry);          // list/map/string/print, etc.

// Compile (pass the shared TypeCache and registry for type consistency)
ModuleRegistry modules(allocator);
SemanticAnalyzer analyzer(allocator, types, modules, &registry);
analyzer.analyze(program);
IRBuilder ir_builder(allocator, types, registry, analyzer.symbols(), modules);
BCModule* module = BytecodeBuilder().build(ir_builder.build(program));
registry.apply_to_module(module);            // wire natives into the runtime

// Execute
RoxyVM vm; vm_init(&vm);
vm_load_module(&vm, module);
vm_call(&vm, "test", {});
```

```roxy
fun test(): i32 {
    var p = Point { x = 3, y = 4 };
    return p.sum();   // returns 7
}
```

## Files

| File | Purpose |
|------|---------|
| `include/roxy/vm/binding/type_traits.hpp` | `RoxyType<T>` mappings |
| `include/roxy/vm/binding/function_traits.hpp` | Compile-time signature extraction |
| `include/roxy/vm/binding/binder.hpp` | `FunctionBinder` wrapper generation |
| `include/roxy/vm/binding/registry.hpp` | `NativeRegistry` (declarations + templates) |
| `src/roxy/vm/binding/registry.cpp` | `NativeRegistry` non-template implementations |
| `include/roxy/vm/binding/roxy_string.hpp` | `RoxyString` / `RoxyList<T>` wrappers + `RoxyType` specializations |
| `include/roxy/vm/binding/interop.hpp` | Convenience header |
| `include/roxy/vm/natives.hpp`, `src/roxy/vm/natives.cpp` | Built-in natives (incl. `List<T>` / `Map<K, V>` registration) |

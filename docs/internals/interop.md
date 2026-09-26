# Native Functions and C++ Interop

Roxy binds C++ functions with type-safe, automatically generated wrappers. A single bound C++ function works unchanged in both VM mode (registered as a `NativeFunction`, called by `CALL_NATIVE`) and AOT mode (the C emitter emits a typed direct call). The binding system lives in `include/roxy/vm/binding/`.

## Calling Convention

In VM mode a native is a low-level `NativeFunction` (`vm/bytecode.hpp`) invoked by `CALL_NATIVE`: arguments sit in the caller's registers `dst+1, dst+2, ...` and the result is written to `dst`. Embedders rarely write these by hand — `bind<>` generates them.

## Bound Functions Take Only Their Logical Args

Embedder-facing C++ functions are plain `Ret(Args...)` — **no `RoxyVM*` prefix**. Functions that need runtime state (allocator, intern table, embedder `user_data`) call `roxy_get_ctx()` directly; the interpreter activates the active VM's context via `roxy::ScopedContext` on every entry, so the context is always reachable.

```cpp
i32 my_add(i32 a, i32 b) { return a + b; }
f64 my_sqrt(f64 x) { return std::sqrt(x); }

registry.bind<my_add>("add");
registry.bind<my_sqrt>("sqrt");
```

`FunctionBinder<FnPtr>` (`binder.hpp`) generates the `NativeFunction` wrapper at compile time, converting each argument and the result through `RoxyType<T>` (`type_traits.hpp`), which maps a C++ type to its Roxy type and to/from a 64-bit register. Supported C++ types are exactly its specializations.

### AOT Mode

In AOT mode the C emitter consults the same `NativeRegistry` (via `CEmitterConfig::native_registry`) and emits a typed direct call to the entry's `aot_symbol_name` (defaults to the registered Roxy name; `bind<FnPtr>(roxy_name, aot_symbol)` lets them diverge). The emitter pre-scans the IR and writes `extern Ret name(Args...);` declarations in the source preamble, so the binary links against either an inline-defined header (`CEmitterConfig::native_include_paths`) or a separately compiled `.cpp` translation unit.

## NativeRegistry

`NativeRegistry` (`registry.hpp`) is the single registration entry point for functions, methods, constructors, native structs, and generic native types. Its `apply_*` methods push registrations into semantic analysis (`SemanticAnalyzer` takes an optional `NativeRegistry*` and applies them in passes 0c/1.5/1.6, before function bodies are checked) and into the runtime (`apply_to_module`).

## String-Based Binding

The primary registration path uses Roxy signature strings. The registry parses each string through the actual Roxy Lexer + Parser (prepending `native ` and appending `;`), so the full type syntax is supported; the resulting `TypeExpr` nodes are stored on the entry and resolved to concrete `Type*` later.

```cpp
// Free function — name extracted from the signature
registry.bind_native(native_str_concat, "fun str_concat(a: string, b: string): string");

// Name override — e.g. $$-mangled trait method names
registry.bind_native("i32$$hash", native_i32_hash, "fun hash(val: i32): u64");

// One member of an OVERLOAD SET (see overloading.md): keyed by the
// "$ol$print$i32" mangle, grouped by the parsed name into one overload
// chain at symbol registration; the 3rd arg names the AOT C symbol.
// Parameter types must be simple named types.
registry.bind_native_overload(native_print_i32, "fun print(v: i32)", "roxy_print_i32");

// Method on a concrete type
registry.bind_method(native_product, "fun Point.product(): i32");

// Method on a generic type — type params resolved at instantiation
registry.bind_method(native_list_push, "fun List<T>.push(val: T)");

// Constructor — min_args controls optional parameters
registry.bind_constructor(native_map_init, "fun Map<K, V>.new(key_kind: i32, capacity: i32)", 1);
```

A signature-bound wrapper is a raw `NativeFunction`; a method receives `self` as `regs[first_arg]` (a pointer to the struct), followed by the other arguments. Use signature binding when a native needs direct register access (e.g. generic element types).

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

Method entries are stored under the same `$$` mangle the IR builder uses for Roxy-defined methods (`Point$$scaled`); the builder finds the mangled name in the registry and emits `CallNative` instead of `Call`.

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

`List<T>` and `Map<K, V>` are registered this way in `register_builtin_natives` (`src/roxy/vm/natives.cpp`), along with the string and `print` natives.

## Interop Wrappers

`RoxyString` (`roxy_string.hpp`), `RoxyList<T>` (`roxy_list.hpp`) and `RoxyMap<K, V>` (`roxy_map.hpp`) are aliases of the runtime's `roxy::String` / `roxy::List<T>` / `roxy::Map<K, V>` (`roxy_rt.h`) — thin non-owning typed wrappers around a Roxy data pointer that allocate through the active context. Their `RoxyType` specializations let them appear directly as bound-function parameters and return types. Methods: see `roxy_rt.h`.

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

## End-to-End Usage

```cpp
BumpAllocator allocator(8192);
TypeEnv type_env(allocator);
NativeRegistry registry(allocator, type_env.types());

registry.register_struct("Point", {{"x", NativeTypeKind::I32}, {"y", NativeTypeKind::I32}});
registry.bind_method<point_sum>("Point", "sum");
register_builtin_natives(registry);          // list/map/string/print, etc.

// Compile (pass the shared TypeEnv and registry for type consistency). The IR
// passes run in the same order as Compiler::link_modules().
ModuleRegistry modules(allocator);
SemanticAnalyzer analyzer(allocator, type_env, modules, &registry);
analyzer.analyze(program);
IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
IRModule* ir_module = ir_builder.build(program);
coroutine_lower(ir_module, allocator, type_env);
optimize_module(ir_module, allocator);
BCModule* module = BytecodeBuilder().build(ir_module);
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

Binding machinery: `include/roxy/vm/binding/` (`registry.{hpp,cpp}`, `binder.hpp`, `type_traits.hpp`, wrapper headers; `interop.hpp` is the convenience include). Built-in natives: `vm/natives.{hpp,cpp}`.

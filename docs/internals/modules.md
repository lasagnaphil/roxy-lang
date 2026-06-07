# Module System

Roxy organizes code across multiple files with `import`/`from` syntax, `pub` export visibility, and native (C++) module integration. Modules are compiled together and statically linked into a single bytecode module — cross-module calls carry no runtime resolution overhead.

## Import Syntax

### Direct import

Import a module and access its exports via qualified names:

```roxy
import math;

fun main(): i32 {
    return math.square(5);  // qualified access
}
```

### Selective import (`from ... import`)

Pull specific symbols directly into the current scope, optionally renamed with `as`:

```roxy
from math import sin, cos;
from utils import add as util_add;

fun main(): f64 {
    return sin(0.5) + cos(0.5);
}
```

## Export Visibility

Functions marked `pub` are exported and importable by other modules; unmarked functions are private to their module:

```roxy
pub fun double(x: i32): i32 { return x * 2; }  // exported
fun helper(): i32 { return 42; }               // private
```

## Builtin Prelude

Built-in functions live in a special `"builtin"` module (`BUILTIN_MODULE_NAME`, `vm/natives.hpp`) auto-imported as a prelude, so they are available without any explicit import:

- `print(s: string)` — print string to stdout
- `str_concat(a: string, b: string): string` — concatenate strings
- `str_eq(a: string, b: string): bool` — string equality
- `str_ne(a: string, b: string): bool` — string inequality
- `str_len(s: string): i32` — string length

```roxy
fun main(): i32 {
    print("hello");   // no import needed
    return 0;
}
```

## Architecture

The module layer is built from a few data structures in `compiler/module_registry.hpp`:

- **`ModuleExport`** — a single export entry: name, `ExportKind` (Function / Struct / Enum), `Type*`, plus `is_native` / `is_pub` flags, export index, and the AST `Decl*` (null for natives).
- **`ModuleInfo`** — module metadata: name, the list of `ModuleExport`s, and (for native modules) the backing `NativeRegistry*`.
- **`ModuleRegistry`** — central registry of all modules. Registers script modules (`register_script_module`) and native modules (`register_native_module`), and resolves imports (`find_module`, `find_export`).

C++ binding via `NativeRegistry` (`vm/binding/registry.hpp`) is a separate concern; see [interop.md](interop.md).

## Multi-Module Compilation

The `Compiler` class (`compiler/compiler.hpp`) drives multi-file compilation. Callers register native registries and add named sources, then call `compile()`, which returns a linked `BCModule*` (null on failure, with errors available via `errors()`):

```cpp
Compiler compiler(allocator);
compiler.add_native_registry("math", &math_registry);
compiler.add_source("utils", utils_source, utils_len);
compiler.add_source("main", main_source, main_len);
BCModule* module = compiler.compile();
```

### Pipeline

1. **Parse** all modules into ASTs.
2. **Topological sort** modules by import dependency.
3. **Detect cycles** during the sort — circular imports are a compile error.
4. **Semantic analysis** in dependency order, registering each module's exports.
5. **Build IR** (SSA) for all modules.
6. **Link** — merge all functions into a single `IRModule` / `BCModule`, resolving cross-module calls.

### Circular import detection

The topological sort uses DFS-based cycle detection. Mutually importing modules fail:

```roxy
// module_a.roxy        // module_b.roxy
import b;               import a;   // ERROR: circular import
```

Error message: `Circular import detected: module 'b' imports 'a' which creates a cycle`.

## Cross-Module Calls (Static Linking)

A call to an imported function lowers to `IROp::CallExternal`, which records the target module name, function name, and arguments (`CallExternalData`, emitted in `ir_builder.cpp`).

Because all modules are linked statically, these are resolved entirely at compile time. `Compiler::link_modules()` merges every module's IR functions into one `IRModule` and builds a function-name → index map (`m_func_indices`) during lowering. Each `CallExternal` looks up its target in that map and is lowered to a regular `CALL` (or `CALL_NATIVE` for natives) with the resolved index. The result: no runtime resolution overhead, all function indices known at compile time, and a bytecode module containing only `CALL` / `CALL_NATIVE` opcodes.

## Semantic Analysis Integration

Imports are processed in Pass 0 of semantic analysis, before type declarations: first the builtin prelude is auto-imported, then user `import` declarations are resolved against the `ModuleRegistry`.

Resolving a qualified `module.function()` access checks that the left-hand side is a module symbol, looks up the export, verifies it is `pub`, and returns the export's type for type checking.

## Native Module Integration

A native module is a `NativeRegistry` of C++ functions exposed to Roxy. Bound functions take **no `RoxyVM*` parameter** — a function that needs runtime state calls `roxy_get_ctx()`:

```cpp
NativeRegistry math_registry(allocator, types);
math_registry.bind<math_sin>("sin");   // f64 math_sin(f64 x)
math_registry.bind<math_cos>("cos");   // f64 math_cos(f64 x)

ModuleRegistry modules(allocator);
modules.register_native_module("math", &math_registry, types);
```

`register_native_module` creates a `ModuleInfo`, then iterates the registry's entries, creating a `ModuleExport` for each native function with `is_native = true` and `is_pub = true`.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/module_registry.hpp` | `ModuleInfo`, `ModuleExport`, `ModuleRegistry` |
| `src/roxy/compiler/module_registry.cpp` | module registration, native-module conversion |
| `include/roxy/compiler/compiler.hpp` | `Compiler` class declaration |
| `src/roxy/compiler/compiler.cpp` | multi-module compilation, topological sort, linking |
| `include/roxy/vm/natives.hpp` | `BUILTIN_MODULE_NAME` constant |
| `src/roxy/compiler/semantic.cpp` | import analysis, prelude auto-import, qualified access |
| `src/roxy/compiler/ir_builder.cpp` | `CallExternal` IR emission |
| `src/roxy/compiler/lowering.cpp` | static linking (`CallExternal` → `CALL`) |
| `tests/e2e/test_modules.cpp` | module system E2E tests |

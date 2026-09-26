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

Built-in functions live in a special `"builtin"` module (`BUILTIN_MODULE_NAME`, `vm/natives.hpp`) auto-imported as a prelude, so they are available without any explicit import. `register_builtin_natives` in `src/roxy/vm/natives.cpp` is the authoritative list (`print` overloads, `str_*`, primitive `$$to_string`/`$$hash`, `List`/`Map`, misc). The `__list_*` / `__map_*` helpers registered there are compiler-internal, not user-callable.

## Architecture

`ModuleRegistry` (`compiler/driver/module_registry.hpp`) holds a `ModuleInfo` per script or native module, each listing its `ModuleExport`s, and resolves imports against them. C++ binding itself is [interop.md](interop.md)'s concern.

## Multi-Module Compilation

The `Compiler` class (`compiler/driver/compiler.hpp`) drives multi-file compilation; `compile()` returns a linked `BCModule*` (null on failure, errors via `errors()`):

```cpp
Compiler compiler(allocator);
compiler.add_native_registry("math", &math_registry);
compiler.add_source("utils", utils_source, utils_len);
compiler.add_source("main", main_source, main_len);
BCModule* module = compiler.compile();
```

Modules are parsed, topologically sorted by import dependency (a DFS; **circular imports are a compile error**), analyzed in dependency order, built to IR, then linked into one `IRModule` / `BCModule`.

## Cross-Module Calls (Static Linking)

A call to an imported function is emitted as `IROp::CallExternal` (target module + function name). `Compiler::link_modules()` merges all modules' IR into one `IRModule`, and lowering resolves each `CallExternal` by name to a plain `CALL` (or `CALL_NATIVE`) — so the bytecode has no external-call opcode and no runtime resolution.

## Semantic Analysis Integration

Imports are processed in Pass 0 of semantic analysis, before type declarations: the builtin prelude first, then user imports. A qualified `module.function()` access must name a `pub` export.

## Native Module Integration

A native module is a `NativeRegistry` of C++ functions exposed to Roxy. Bound functions take **no `RoxyVM*` parameter** — a function that needs runtime state calls `roxy_get_ctx()`:

```cpp
NativeRegistry math_registry(allocator, types);
math_registry.bind<math_sin>("sin");   // f64 math_sin(f64 x)
math_registry.bind<math_cos>("cos");   // f64 math_cos(f64 x)

ModuleRegistry modules(allocator);
modules.register_native_module("math", &math_registry, types);
```

Every native function in the registry becomes a `pub` export of that module.

## Files

`compiler/driver/{module_registry,compiler}.{hpp,cpp}`; import analysis in `sema/semantic.cpp`. Tests: `tests/e2e/test_modules.cpp`.

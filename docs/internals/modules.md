# Module System

Roxy supports a module system for organizing code across multiple files, importing native (C++) functions, and controlling symbol visibility.

## Import Syntax

### Direct Import (`import`)

Import a module and access its exports via qualified names:

```roxy
import math;

fun main(): i32 {
    return math.square(5);  // Qualified access
}
```

### Selective Import (`from ... import`)

Import specific symbols directly into the current scope:

```roxy
from math import sin, cos;

fun main(): f64 {
    return sin(0.5) + cos(0.5);  // Direct access
}
```

### Import with Alias

Rename imported symbols to avoid conflicts:

```roxy
from math import add as math_add;
from utils import add as util_add;

fun main(): i32 {
    return math_add(1, 2) + util_add(3, 4);
}
```

## Export Visibility

Functions marked `pub` are exported and can be imported by other modules:

```roxy
// utils.roxy
pub fun double(x: i32): i32 {
    return x * 2;
}

fun helper(): i32 {  // Not exported (private)
    return 42;
}
```

## Builtin Prelude

Built-in functions are automatically available without explicit imports. They are registered in a special `"builtin"` module that is auto-imported as a prelude.

**Built-in functions:**
- `print(value: i32)` - Print integer to stdout
- `print_str(s: string)` - Print string to stdout
- `str_concat(a: string, b: string): string` - Concatenate strings
- `str_eq(a: string, b: string): bool` - String equality
- `str_ne(a: string, b: string): bool` - String inequality
- `str_len(s: string): i32` - Get string length

These can be used without imports:

```roxy
fun main(): i32 {
    print(42);           // No import needed
    print_str("hello");  // No import needed
    return 0;
}
```

## Architecture

### Key Components

| Component | File | Description |
|-----------|------|-------------|
| `ModuleRegistry` | `module_registry.hpp` | Central registry of all modules |
| `ModuleInfo` | `module_registry.hpp` | Module metadata and exports list |
| `ModuleExport` | `module_registry.hpp` | Single export entry (function, struct, or enum) |
| `NativeRegistry` | `binding/registry.hpp` | C++ function binding (separate concern) |
| `Compiler` | `compiler.hpp` | Multi-module compiler |

### Data Structures

```cpp
// Export kinds
enum class ExportKind : u8 {
    Function,
    Struct,
    Enum,
};

// Single export from a module
struct ModuleExport {
    StringView name;        // Symbol name
    ExportKind kind;        // Function/Struct/Enum
    Type* type;             // Type (function type for functions)
    bool is_native;         // True if C++ native function
    bool is_pub;            // True if publicly visible
    u32 index;              // Index in module's export array
    Decl* decl;             // AST declaration (nullptr for native)
};

// Module metadata
struct ModuleInfo {
    StringView name;                // Module name
    Vector<ModuleExport> exports;   // All exports
    NativeRegistry* natives;        // For native modules (nullptr for script)
    bool is_native;                 // True if native-only module
};
```

### ModuleRegistry

The `ModuleRegistry` tracks all modules and provides import resolution:

```cpp
class ModuleRegistry {
public:
    // Register a native C++ module
    void register_native_module(StringView name, NativeRegistry* natives, TypeCache& types);

    // Register a script module (exports populated during analysis)
    ModuleInfo* register_script_module(StringView name);

    // Lookup
    ModuleInfo* find_module(StringView name) const;
    const ModuleExport* find_export(ModuleInfo* module, StringView name) const;
};
```

## Multi-Module Compilation

### Compiler Class

The `Compiler` class handles multi-file compilation:

```cpp
Compiler compiler(allocator);

// Add native modules (optional)
compiler.add_native_registry("math", &math_registry);

// Add source modules
compiler.add_source("utils", utils_source, utils_len);
compiler.add_source("main", main_source, main_len);

// Compile and link
BCModule* module = compiler.compile();
if (!module) {
    for (const char* err : compiler.errors()) {
        printf("Error: %s\n", err);
    }
}
```

### Compilation Pipeline

1. **Parse all modules** - Generate ASTs for each source file
2. **Topological sort** - Order modules by import dependencies
3. **Detect cycles** - Report error if circular imports found
4. **Semantic analysis** - Type check in dependency order, register exports
5. **Build IR** - Generate SSA IR for all modules
6. **Link** - Merge all functions into single BCModule, resolve cross-module calls

### Circular Import Detection

The compiler uses DFS-based cycle detection during topological sort:

```roxy
// module_a.roxy
import b;
pub fun foo(): i32 { return 1; }

// module_b.roxy
import a;  // ERROR: Circular import detected
pub fun bar(): i32 { return 2; }
```

Error message: `Circular import detected: module 'b' imports 'a' which creates a cycle`

## Cross-Module Calls

### IR Representation

Cross-module function calls use `IROp::CallExternal`:

```cpp
struct CallExternalData {
    StringView module_name;  // Target module
    StringView func_name;    // Function name in module
    Span<ValueId> args;      // Arguments
};
```

### Bytecode (Static Linking)

Cross-module calls are resolved at compile-time through static linking:

1. All IR modules are merged into a single `IRModule` by `Compiler::link_modules()`
2. The merged IR contains all functions from all script modules
3. During lowering, `IROp::CallExternal` instructions look up the target function
   in `m_func_indices` (which contains all merged functions)
4. A regular `CALL` instruction is emitted with the resolved function index

This approach means:
- No runtime resolution overhead for cross-module calls
- All function indices are known at compile time
- The bytecode module contains only `CALL` and `CALL_NATIVE` opcodes

### Linking

During the link phase in `Compiler::link_modules()`:
1. All IR functions from all modules are collected into a merged `IRModule`
2. A function name → index map is built during bytecode lowering
3. `IROp::CallExternal` instructions are lowered to regular `CALL` bytecode

## Semantic Analysis Integration

### Import Processing

Imports are processed in Pass 0 of semantic analysis, before type declarations:

```cpp
bool SemanticAnalyzer::analyze(Program* program) {
    // Pass 0a: Auto-import builtin prelude
    import_builtin_prelude();

    // Pass 0b: Process user imports
    for (Decl* decl : program->declarations) {
        if (decl->kind == AstKind::DeclImport) {
            analyze_import_decl(decl);
        }
    }

    // Pass 1+: Continue with type declarations, etc.
}
```

### Qualified Access Resolution

When analyzing `module.function()` expressions, the semantic analyzer:

1. Checks if the left-hand side is a module symbol
2. Looks up the export in the module
3. Verifies visibility (must be `pub`)
4. Returns the export's type for type checking

## Native Module Integration

Native modules are C++ function registries exposed to Roxy:

```cpp
// Create registry and bind functions (all take RoxyVM* as first parameter)
NativeRegistry math_registry(allocator, types);
math_registry.bind<math_sin>("sin");    // f64 math_sin(RoxyVM* vm, f64 x)
math_registry.bind<math_cos>("cos");    // f64 math_cos(RoxyVM* vm, f64 x)

// Register as a module
ModuleRegistry modules(allocator);
modules.register_native_module("math", &math_registry, types);
```

The `register_native_module` function:
1. Creates a `ModuleInfo` for the module
2. Iterates through the `NativeRegistry` entries
3. Creates `ModuleExport` entries for each native function
4. Marks all exports as `is_native = true` and `is_pub = true`

## File Reference

| File | Description |
|------|-------------|
| `include/roxy/compiler/module_registry.hpp` | ModuleInfo, ModuleExport, ModuleRegistry |
| `src/roxy/compiler/module_registry.cpp` | Module registration, native module conversion |
| `include/roxy/compiler/compiler.hpp` | Compiler class declaration |
| `src/roxy/compiler/compiler.cpp` | Multi-module compilation, topological sort, linking |
| `include/roxy/vm/natives.hpp` | BUILTIN_MODULE_NAME constant |
| `src/roxy/compiler/semantic.cpp` | Import analysis, prelude auto-import |
| `src/roxy/compiler/ir_builder.cpp` | CallExternal IR emission |
| `src/roxy/compiler/lowering.cpp` | Static linking (CallExternal → CALL) |
| `tests/e2e/modules_test.cpp` | Module system E2E tests |

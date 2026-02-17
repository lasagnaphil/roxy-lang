# Claude Code Project Guide - Roxy

## Project Overview

Roxy is an embeddable scripting language for game engines with:
- Static typing
- Value semantics by default
- Memory management via `uniq`/`ref`/`weak` references (no GC)
- Fast C++ interop
- Future AOT compilation to C

## Build System

- **Build tool:** CMake with Ninja
- **Compiler:** clang-cl (Windows), clang/gcc (macOS/Linux)
- **C++ Standard:** C++17

### Build Commands

**Windows (clang-cl):**
```bash
cd build
cmake .. -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MT="C:/Program Files/LLVM/bin/llvm-mt.exe"
ninja
```

**macOS/Linux:**
```bash
cd build
cmake .. -G Ninja -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ninja
```

**With AddressSanitizer:**
```bash
cmake .. -G Ninja -DENABLE_ASAN=ON
```

### CMake Libraries

The project is organized into 4 libraries:
- `roxy_core` - File utilities, rx::String, rx::format_to
- `roxy_shared` - Lexer and tokens
- `roxy_compiler` - Parser, AST, semantic analysis, SSA IR, IR builder
- `roxy_vm` - Bytecode, value, object, VM, interpreter, lowering

## Code Conventions

- **Namespace:** `rx::`
- **Naming:**
  - Functions/methods: `snake_case`
  - Types/classes: `PascalCase`
  - Member variables: `m_` prefix (e.g., `m_source`, `m_current`)
- **Types:** Use aliases from `types.hpp`: `u8`, `u16`, `u32`, `u64`, `i8`, `i16`, `i32`, `i64`, `f32`, `f64`
- **Headers:** Use `#pragma once`
- **Assertions:** Use `assert()` for invariants

## Project Structure

```
roxy-v2/
├── include/roxy/
│   ├── core/           # Core utilities (types.hpp, span.hpp, vector.hpp, allocators)
│   ├── shared/         # Lexer and tokens
│   ├── compiler/       # Parser, AST, types, semantic, SSA IR, IR builder, lowering
│   └── vm/             # Bytecode, value, object, VM, interpreter, binding/
├── src/roxy/           # Implementation files matching include/ structure
├── tests/
│   ├── test_main.cpp   # Single doctest entry point
│   ├── unit/           # Unit tests (lexer, parser, semantic, IR, bytecode, VM)
│   └── e2e/            # End-to-end tests (basics, structs, lists, strings, modules, etc.)
├── docs/
│   ├── overview.md     # Language features and design
│   ├── grammar.md      # Grammar specification, numeric literals, type casting
│   ├── libraries.md    # Vendored library documentation
│   └── internals/      # Detailed implementation documentation
└── CMakeLists.txt
```

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Lowering → Bytecode → VM
```

## Key Language Features

### Reference Types

| Type | Owns? | Nullable? | On dangling |
|------|-------|-----------|-------------|
| `uniq` | Yes | No | N/A (is owner) |
| `ref` | No | No | Assert/crash |
| `weak` | No | Yes | Returns null or asserts |

### Keywords

- Types/modifiers: `true false nil var fun struct enum pub native`
- Control flow: `if else for while break continue return when case`
- OOP: `self super new delete`
- References: `uniq ref weak out inout`
- Imports: `import from`

### Numeric Types and Casting

See `docs/grammar.md` for numeric literal suffixes and type casting rules.

## Implemented Components

### Frontend
**Lexer** - Tokenizes source code with number bases, suffixes, escape sequences, nested comments.
**Details:** `docs/internals/frontend.md` | **Files:** `shared/lexer.hpp`, `shared/lexer.cpp`

**Parser** - Recursive descent with Pratt parsing for expressions. Fail-fast design.
**Details:** `docs/internals/frontend.md` | **Files:** `compiler/parser.hpp`, `compiler/parser.cpp`

**AST** - 15 expression types, 9 statement types, 7 declaration types.
**Files:** `compiler/ast.hpp`

**Semantic Analysis** - Multi-pass analyzer with symbol resolution, type inference, type checking.
**Details:** `docs/internals/frontend.md` | **Files:** `compiler/semantic.hpp`, `compiler/semantic.cpp`

### Type System
**Types** - Primitives (`void`, `bool`, `i32`, `i64`, `f32`, `f64`, `string`), structs, enums, references.
**Files:** `compiler/types.hpp`, `compiler/types.cpp`

**Enums** - C-style enumerations with integer underlying type. Access via `Type::Variant`.
**Tests:** `tests/e2e/enums_test.cpp`

**Structs** - Stack-allocated value types with slot-based layout, inheritance, methods, constructors/destructors.
**Details:** `docs/internals/structs.md`, `docs/internals/methods.md`, `docs/internals/inheritance.md`, `docs/internals/constructors.md`

**Tagged Unions** - Discriminated unions with `when` clause in struct definitions.
**Details:** `docs/internals/tagged-unions.md` | **Tests:** `tests/e2e/tagged_unions_test.cpp`

### IR and Bytecode
**SSA IR** - Block arguments (not phi nodes), 43+ operations for all basic operations.
**Details:** `docs/internals/ssa-ir.md` | **Files:** `compiler/ssa_ir.hpp`, `compiler/ir_builder.hpp`

**Bytecode** - 32-bit fixed-width register-based, 64+ opcodes, three instruction formats (ABC, ABI, AOFF).
**Details:** `docs/internals/bytecode.md` | **Files:** `vm/bytecode.hpp`, `compiler/lowering.hpp`

### Runtime
**VM** - Shared register file with windowing, call frame stack, module loading.
**Details:** `docs/internals/vm.md` | **Files:** `vm/vm.hpp`, `vm/interpreter.hpp`

**Lists** - Dynamic lists (`List<T>`) with bounds checking, push/pop/len/cap methods.
**Details:** `docs/internals/arrays.md` | **Files:** `vm/list.hpp`

**Strings** - Heap-allocated string objects. Operations via native functions (`str_concat`, `str_eq`, `str_len`). F-string interpolation (`f"hello {expr}"`) with automatic `to_string` conversion via builtin `Printable` trait.
**Details:** `docs/internals/strings.md` | **Files:** `vm/string.hpp`

**Slab Allocator** - Custom allocator with Vale-style random generational references, tombstoning.
**Details:** `docs/internals/memory.md` | **Files:** `vm/slab_allocator.hpp`, `vm/vmem.hpp`

### Interop and Modules
**C++ Interop** - Type-safe function binding with automatic wrapper generation via `NativeRegistry`.
**Details:** `docs/internals/interop.md` | **Files:** `vm/binding/`

**Module System** - Multi-file compilation with `import`/`from` syntax, topological sorting, static linking.
**Details:** `docs/internals/modules.md` | **Files:** `compiler/module_registry.hpp`, `compiler/compiler.hpp`

### Control Flow
**When Statement** - Pattern matching on enum values with phi node support for variable modifications.
**Tests:** `tests/e2e/when_test.cpp`

### Traits
**Traits** - Ad-hoc polymorphism with trait declarations, required/default methods, `for Trait` implementations, trait inheritance, `Self` type, operator dispatch (arithmetic, comparison, bitwise, unary, indexing) for structs, primitives, and lists via unified `TypeCache::lookup_method()`, and generic traits with type parameters (`trait Add<Rhs>`, `for Mul<i32>`).
**Details:** `docs/internals/traits.md`, `docs/internals/operator-overloading.md` | **Tests:** `tests/e2e/traits_test.cpp`

### Generics
**Generics** - Parametric polymorphism with monomorphization. Generic functions (`fun identity<T>(v: T): T`) and generic structs (`struct Box<T> { value: T; }`). Supports local type inference from function arguments and struct field values (`identity(42)` infers T=i32, `Box { value = 42 }` infers T=i32). Explicit type arguments also supported. Angle bracket syntax with trial-parse disambiguation.
**Details:** `docs/internals/generics.md` | **Tests:** `tests/e2e/generics_test.cpp`

## Planned Components (Not Yet Implemented)

- C backend (AOT compilation via SSA IR → C transpilation, see `docs/internals/c-backend.md`)
- LSP parser (error recovery, lossless CST)
- LSP server features (completion, hover, go-to-definition)
- Optimizations

## Testing

- **Framework:** doctest (vendored in `include/roxy/core/doctest/`)
- **Single executable:** `roxy_tests` contains all unit and E2E tests
- **Helpers:** `tests/e2e/test_helpers.hpp` provides `compile()`, `compile_and_run()`, `run_and_capture()`

### Running Tests

```bash
cd build
./roxy_tests                              # Run all tests
./roxy_tests --test-case-exclude="E2E*"   # Run only unit tests
./roxy_tests --test-case="E2E*"           # Run only E2E tests
./roxy_tests --test-case="E2E - Struct*"  # Run specific category
./roxy_tests --list-test-cases            # List all test cases
```

On Windows, use `.exe` extension.

## Documentation

- `CLAUDE.md` - Quick reference for Claude Code (this file)
- `docs/overview.md` - Language design philosophy and roadmap
- `docs/grammar.md` - Grammar specification, numeric literals, type casting
- `docs/libraries.md` - Vendored library documentation
- `docs/internals/` - Detailed implementation documentation:
  - `vm.md` - VM state, interpreter loop, value representation
  - `bytecode.md` - Instruction encoding, opcode reference
  - `ssa-ir.md` - Block arguments, lowering to bytecode
  - `memory.md` - Reference types, object header, slab allocator
  - `structs.md` - Stack-allocated structs, slot-based layout, struct parameters/returns
  - `arrays.md` - Dynamic lists (`List<T>`), bounds checking
  - `strings.md` - String objects, concatenation, comparison
  - `interop.md` - Native functions, automatic C++ binding
  - `frontend.md` - Lexer, parser, semantic analysis
  - `modules.md` - Module system, imports, multi-file compilation
  - `constructors.md` - Named constructors/destructors, `self` keyword
  - `methods.md` - Struct methods, `self` parameter, name mangling
  - `inheritance.md` - Struct inheritance, subtyping, `super` keyword
  - `tagged-unions.md` - Discriminated unions with `when` clause
  - `traits.md` - Traits: declarations, required/default methods, trait inheritance, operator dispatch
  - `operator-overloading.md` - Operator traits (arithmetic, comparison, bitwise, unary) with unified primitive/struct dispatch
  - `generics.md` - Generic functions and structs with monomorphization
  - `c-backend.md` - C backend design plan (AOT compilation via SSA IR → C)

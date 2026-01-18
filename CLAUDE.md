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
cmake .. -G Ninja
ninja
```

**With AddressSanitizer:**
```bash
cmake .. -G Ninja -DENABLE_ASAN=ON
```

### CMake Libraries

The project is organized into 4 libraries:
- `roxy_core` - File utilities + fmt library
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
│   ├── core/                    # Core utilities and vendored libs
│   │   ├── types.hpp            # Type aliases (u32, i64, f64, etc.)
│   │   ├── span.hpp             # Non-owning array view
│   │   ├── vector.hpp           # Dynamic array
│   │   ├── string_view.hpp      # String view
│   │   ├── bump_allocator.hpp   # Arena allocator
│   │   ├── unique_ptr.hpp       # Unique pointer
│   │   ├── array.hpp            # Fixed-size array
│   │   ├── function_ref.hpp     # Function reference wrapper
│   │   ├── pair.hpp             # Pair type
│   │   ├── binary_search.hpp    # Binary search utilities
│   │   ├── pseudorandom.hpp     # Random number generation
│   │   ├── file.hpp             # File I/O
│   │   ├── doctest/             # Vendored doctest testing framework
│   │   ├── fmt/                 # Vendored fmt formatting library
│   │   └── tsl/                 # Vendored robin map/set
│   │
│   ├── shared/                  # Shared frontend components
│   │   ├── token_kinds.hpp      # TokenKind enum (97 lines)
│   │   ├── token.hpp            # Token, SourceLocation structs
│   │   └── lexer.hpp            # Lexer class
│   │
│   ├── compiler/                # Compiler pipeline
│   │   ├── ast.hpp              # AST node definitions (405 lines)
│   │   ├── parser.hpp           # Recursive descent parser
│   │   ├── types.hpp            # Type system (261 lines)
│   │   ├── symbol_table.hpp     # Scope and symbol management
│   │   ├── semantic.hpp         # Semantic analysis / type checking
│   │   ├── ssa_ir.hpp           # SSA IR with block arguments (276 lines)
│   │   ├── ir_builder.hpp       # AST to SSA IR conversion
│   │   └── lowering.hpp         # SSA to bytecode lowering
│   │
│   └── vm/                      # Virtual machine
│       ├── bytecode.hpp         # Opcode definitions (284 lines)
│       ├── value.hpp            # Value representation (135 lines)
│       ├── object.hpp           # Object header and ref counting
│       ├── vm.hpp               # VM state and API
│       └── interpreter.hpp      # Interpreter loop
│
├── src/roxy/
│   ├── core/
│   │   └── file.cpp
│   ├── shared/
│   │   ├── token_kinds.cpp      # token_kind_to_string()
│   │   └── lexer.cpp            # Lexer implementation (454 lines)
│   ├── compiler/
│   │   ├── parser.cpp           # Parser implementation (1,103 lines)
│   │   ├── types.cpp            # Type system implementation
│   │   ├── symbol_table.cpp     # Symbol table implementation
│   │   ├── semantic.cpp         # Semantic analysis (1,152 lines)
│   │   ├── ssa_ir.cpp           # SSA IR implementation (390 lines)
│   │   ├── ir_builder.cpp       # IR builder implementation
│   │   └── lowering.cpp         # Lowering implementation (536 lines)
│   └── vm/
│       ├── bytecode.cpp         # Bytecode encoding/decoding
│       ├── value.cpp            # Value operations
│       ├── object.cpp           # Object allocation/ref counting
│       ├── vm.cpp               # VM initialization and execution
│       └── interpreter.cpp      # Interpreter loop (503 lines)
│
├── tests/
│   ├── lexer_test.cpp           # Lexer/token tests
│   ├── parser_test.cpp          # Parser and AST construction
│   ├── semantic_test.cpp        # Type checking and symbol resolution
│   ├── ssa_ir_test.cpp          # IR generation and construction
│   ├── bytecode_test.cpp        # Bytecode encoding/decoding
│   ├── vm_test.cpp              # VM execution and runtime
│   ├── lowering_test.cpp        # SSA to bytecode lowering
│   └── e2e_test.cpp             # End-to-end compilation and execution
│
├── docs/
│   ├── overview.md              # Language features and design
│   └── grammar.md               # Roxy grammar specification
├── DESIGN.md                    # Detailed VM and compiler design
└── CMakeLists.txt
```

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Bytecode Builder → Bytecode → VM
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
- OOP: `this super new delete`
- References: `uniq ref weak out inout`
- Imports: `import from`

## Implemented Components

### Lexer (`include/roxy/shared/lexer.hpp`, `src/roxy/shared/lexer.cpp`)

Tokenizes Roxy source code:
- Decimal, hex (`0xFF`), binary (`0b1010`), octal (`0o77`) number literals
- Integer suffixes (`u`, `l`, `ul`) and float suffix (`f`)
- String literals with escape sequences (`\n`, `\t`, `\\`, `\"`)
- All operators including two-character ones (`::`, `&&`, `||`, `+=`, etc.)
- Line comments (`//`) and nested block comments (`/* */`)
- Keyword recognition via trie-style switch
- Accurate line/column tracking

### Parser (`include/roxy/compiler/parser.hpp`, `src/roxy/compiler/parser.cpp`)

Recursive descent parser with Pratt parsing for expressions:
- Fail-fast design (stops on first error)
- Produces typed AST nodes
- Handles all grammar productions from `docs/grammar.md`

### AST (`include/roxy/compiler/ast.hpp`)

Complete AST node definitions:
- 24 expression types (literals, binary ops, unary ops, calls, etc.)
- 9 statement types (if, while, for, return, block, etc.)
- 5 declaration types (function, variable, struct, enum, import)

### Type System (`include/roxy/compiler/types.hpp`, `src/roxy/compiler/types.cpp`)

Full type system:
- Primitive types: `void`, `bool`, `int`, `float`, `string`
- Struct and enum types with field/variant info
- Reference types: `uniq`, `ref`, `weak`
- Type compatibility and conversion rules

### Semantic Analysis (`include/roxy/compiler/semantic.hpp`, `src/roxy/compiler/semantic.cpp`)

Multi-pass semantic analyzer:
- Symbol resolution with scoped symbol tables
- Type inference and type checking
- Function signature validation
- Error reporting with source locations

### SSA IR (`include/roxy/compiler/ssa_ir.hpp`, `src/roxy/compiler/ssa_ir.cpp`)

SSA IR with block arguments (not phi nodes):
- 30+ IR opcodes covering all basic operations
- ValueId and BlockId for unique identification
- Block parameters for control flow merging
- Clean dataflow representation

### IR Builder (`include/roxy/compiler/ir_builder.hpp`, `src/roxy/compiler/ir_builder.cpp`)

Converts type-checked AST to SSA IR:
- Generates SSA form directly
- Handles control flow (if/else, while, for)
- Proper block argument insertion for loops

### Bytecode (`include/roxy/vm/bytecode.hpp`, `src/roxy/vm/bytecode.cpp`)

32-bit fixed-width register-based bytecode:
- Three instruction formats: ABC (3-operand), ABI (immediate), AOFF (offset)
- 30+ opcodes for arithmetic, logic, control flow, objects
- Constant pool for large values
- BCFunction and BCModule structures

### VM (`include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp`)

Virtual machine core:
- Shared register file with windowing for function calls
- Call frame stack for tracking active functions
- Module loading and function lookup

### Interpreter (`include/roxy/vm/interpreter.hpp`, `src/roxy/vm/interpreter.cpp`)

Switch-based interpreter loop:
- Handles ~35 opcodes
- Division by zero checking
- Error reporting via `vm->error`

### Lowering (`include/roxy/compiler/lowering.hpp`, `src/roxy/compiler/lowering.cpp`)

SSA IR to bytecode conversion:
- Two-pass emission (record block offsets, then patch jumps)
- Block arguments become MOV instructions
- Simple register allocation (SSA value ID = register number)
- Constant pool management

## Partially Implemented (TODOs in code)

- **Field Access** - `GET_FIELD`/`SET_FIELD` opcodes defined and lowering works, interpreter has placeholder
- **Index Access** - `GET_INDEX`/`SET_INDEX` opcodes defined and lowering works, interpreter has placeholder
- **Native Function Calls** - Basic infrastructure exists, full lowering incomplete
- **Method Lookup** - Semantic analysis has placeholder for proper method resolution
- **String Objects** - Basic value representation exists, full handling incomplete

## Planned Components (Not Yet Implemented)

- C++ interop layer (type registration, native binding)
- LSP parser (error recovery, lossless CST)
- LSP server features (completion, hover, go-to-definition)
- Optimizations

## Testing

- **Framework:** doctest (vendored in `include/roxy/core/doctest/`)
- Use `TEST_CASE` and `SUBCASE` for test organization
- Use `CHECK` for assertions, `REQUIRE` for critical checks

### Running Tests

```bash
cd build
./lexer_test && ./parser_test && ./semantic_test && \
./ssa_ir_test && ./bytecode_test && ./vm_test && \
./lowering_test && ./e2e_test
```

On Windows, use `.exe` extension.

## Documentation

- `docs/overview.md` - Language design and features
- `docs/grammar.md` - Formal grammar specification
- `DESIGN.md` - Detailed implementation design (VM, bytecode, interop)
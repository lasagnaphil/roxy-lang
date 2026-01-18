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
│       ├── array.hpp            # Array runtime support
│       ├── natives.hpp          # Built-in native functions
│       ├── vm.hpp               # VM state and API
│       ├── interpreter.hpp      # Interpreter loop
│       └── binding/             # C++ interop system
│           ├── type_traits.hpp      # RoxyType<T> mappings
│           ├── function_traits.hpp  # Compile-time signature extraction
│           ├── binder.hpp           # Automatic wrapper generation
│           ├── registry.hpp         # NativeRegistry class
│           └── interop.hpp          # Convenience header
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
│       ├── array.cpp            # Array allocation and access
│       ├── natives.cpp          # Built-in native function implementations
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
│   ├── e2e_test.cpp             # End-to-end compilation and execution
│   └── interop_test.cpp         # C++ interop and function binding
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
- Array bounds checking
- Error reporting via `vm->error`

### Lowering (`include/roxy/compiler/lowering.hpp`, `src/roxy/compiler/lowering.cpp`)

SSA IR to bytecode conversion:
- Two-pass emission (record block offsets, then patch jumps)
- Block arguments become MOV instructions
- Simple register allocation (SSA value ID = register number)
- Constant pool management
- Native function call lowering

### Arrays (`include/roxy/vm/array.hpp`, `src/roxy/vm/array.cpp`)

Array runtime support:
- `ArrayHeader` struct with length/capacity
- Bounds-checked `array_get`/`array_set` operations
- Integration with object system for memory management
- `GET_INDEX`/`SET_INDEX` opcodes fully implemented

### Native Functions (`include/roxy/vm/natives.hpp`, `src/roxy/vm/natives.cpp`)

Built-in native function support:
- `array_new_int(size)` - Allocate integer array
- `array_len(arr)` - Get array length
- `print(value)` - Print value to stdout
- Registration via `NativeRegistry`
- `CALL_NATIVE` opcode and lowering

### C++ Interop (`include/roxy/vm/binding/`)

Type-safe C++ function binding with automatic wrapper generation:
- **RoxyType<T>** - Maps C++ types to Roxy types at compile time
- **FunctionTraits** - Extracts function signature info
- **FunctionBinder** - Generates native wrappers automatically
- **NativeRegistry** - Unified registration for compile-time and runtime

Example usage:
```cpp
i32 my_add(i32 a, i32 b) { return a + b; }

NativeRegistry registry(allocator, types);
registry.bind<my_add>("add");  // Automatic wrapper generation
registry.apply_to_symbols(symbols);  // For semantic analysis
registry.apply_to_module(module);    // For runtime
```

### Structs (`types.hpp`, `semantic.cpp`, `ir_builder.cpp`, `lowering.cpp`, `interpreter.cpp`)

Stack-allocated value-type structs with packed field layout:
- **Slot-based memory model**: Fields use 4-byte (u32) slots - i32/f32 = 1 slot, i64/f64 = 2 slots
- **Untyped registers**: VM registers are `u64*` (type info for debug mode only)
- **Separate local stack**: Per-function `u32* local_stack` for struct data
- **16-byte aligned frames**: `local_stack_base` aligned to 4 slots for C++ interop
- **SSA IR**: `StackAlloc` instruction allocates struct space, returns pointer
- **Field access**: `GET_FIELD`/`SET_FIELD` opcodes with slot_offset and slot_count
- **Struct literals**: `Point { x = 10, y = 20 }` syntax with field order independence
- **Default values**: Fields with defaults can be omitted from literals
- **Nested structs**: Supported via `GET_FIELD_ADDR` opcode for address computation

Example struct layout:
```
struct Point { x: i32; y: i32; }  → 2 slots (8 bytes)
struct Data { a: i32; b: i64; }   → 3 slots (12 bytes)
```

Example struct literal:
```
struct Config { width: i32 = 800; height: i32 = 600; }
var c = Config { width = 1920 };  // height uses default
```

Key types:
- `FieldInfo.slot_offset` / `FieldInfo.slot_count` - Field position in struct
- `StructTypeInfo.slot_count` - Total slots for struct
- `BCFunction.local_stack_slots` - Stack space needed per function
- `VarDecl.resolved_type` - Type resolved by semantic analysis
- `FieldInit` / `StructLiteralExpr` - AST nodes for struct literals

## Partially Implemented (TODOs in code)

- **Method Lookup** - Semantic analysis has placeholder for proper method resolution
- **String Objects** - Basic value representation exists, full handling incomplete

## Planned Components (Not Yet Implemented)

- Passing structs by value to functions (copy semantics)
- Returning structs from functions
- Heap allocation via `new` and `uniq`/`ref`/`weak`
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
./lowering_test && ./e2e_test && ./interop_test
```

On Windows, use `.exe` extension.

## Documentation

- `docs/overview.md` - Language design and features
- `docs/grammar.md` - Formal grammar specification
- `DESIGN.md` - Architecture overview and implementation status
- `docs/internals/` - Detailed implementation documentation:
  - `vm.md` - VM state, interpreter loop, value representation
  - `bytecode.md` - Instruction encoding, opcode reference
  - `memory.md` - Reference types, object header, ref counting
  - `structs.md` - Stack-allocated structs, slot-based layout
  - `arrays.md` - Dynamic arrays, bounds checking
  - `ssa-ir.md` - Block arguments, lowering to bytecode
  - `interop.md` - Native functions, automatic C++ binding
  - `frontend.md` - Lexer, parser, semantic analysis
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
│       ├── vmem.hpp             # Virtual memory operations interface
│       ├── slab_allocator.hpp   # Slab allocator with tombstoning
│       ├── array.hpp            # Array runtime support
│       ├── string.hpp           # String runtime support
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
│       ├── vmem_win32.cpp       # Windows virtual memory implementation
│       ├── vmem_unix.cpp        # Unix virtual memory implementation
│       ├── slab_allocator.cpp   # Slab allocator implementation
│       ├── array.cpp            # Array allocation and access
│       ├── string.cpp           # String allocation and operations
│       ├── natives.cpp          # Built-in native function implementations
│       ├── vm.cpp               # VM initialization and execution
│       └── interpreter.cpp      # Interpreter loop (503 lines)
│
├── tests/
│   ├── test_main.cpp            # Single doctest entry point
│   ├── unit/                    # Unit tests (8 files)
│   │   ├── lexer_test.cpp       # Lexer/token tests
│   │   ├── parser_test.cpp      # Parser and AST construction
│   │   ├── semantic_test.cpp    # Type checking and symbol resolution
│   │   ├── ssa_ir_test.cpp      # IR generation and construction
│   │   ├── bytecode_test.cpp    # Bytecode encoding/decoding
│   │   ├── vm_test.cpp          # VM execution and runtime
│   │   ├── lowering_test.cpp    # SSA to bytecode lowering
│   │   └── slab_allocator_test.cpp # Slab allocator and weak refs
│   └── e2e/                     # End-to-end tests (9 files)
│       ├── test_helpers.hpp     # Shared compile/run helpers
│       ├── test_helpers.cpp
│       ├── basics_test.cpp      # Variables, arithmetic, control flow
│       ├── recursion_test.cpp   # Recursive functions
│       ├── algorithms_test.cpp  # Complex algorithms, floats
│       ├── arrays_test.cpp      # Array operations, quicksort
│       ├── structs_test.cpp     # Struct fields, literals, params
│       ├── params_test.cpp      # Out/inout parameter tests
│       ├── interop_test.cpp     # C++ interop tests
│       └── strings_test.cpp     # String operations, concatenation
│
├── docs/
│   ├── overview.md              # Language features and design
│   ├── grammar.md               # Roxy grammar specification
│   ├── libraries.md             # Vendored library documentation
│   └── internals/               # Implementation documentation
│       ├── vm.md                # VM state, interpreter loop
│       ├── bytecode.md          # Instruction encoding, opcodes
│       ├── ssa-ir.md            # Block arguments, lowering
│       ├── memory.md            # Reference types, object header
│       ├── structs.md           # Stack-allocated structs
│       ├── arrays.md            # Dynamic arrays
│       ├── interop.md           # C++ function binding
│       └── frontend.md          # Lexer, parser, semantic analysis
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
- 14 expression types (literals, identifiers, binary/unary ops, calls, indexing, field access, assignments, ternary, struct literals)
- 9 statement types (expr, block, if, while, for, return, break, continue, delete)
- 6 declaration types (variable, function, struct, enum, field, import)
- ParamModifier enum (None, Out, Inout) for function parameters

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
- 42 IR operations covering all basic operations:
  - Constants: ConstNull, ConstBool, ConstInt, ConstFloat, ConstString
  - Arithmetic: AddI/F, SubI/F, MulI/F, DivI/F, ModI, NegI/F
  - Comparisons: EqI/F, NeI/F, LtI/F, LeI/F, GtI/F, GeI/F
  - Logical: Not, And, Or; Bitwise: BitAnd, BitOr, BitNot
  - Memory: StackAlloc, GetField, GetFieldAddr, SetField, GetIndex, SetIndex
  - Structs: StructCopy, LoadPtr, StorePtr, VarAddr
  - Calls: Call, CallNative
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
- 58 opcodes organized by category:
  - Constants/Moves: LOAD_NULL, LOAD_TRUE, LOAD_FALSE, LOAD_INT, LOAD_CONST, MOV
  - Arithmetic: ADD_I/F, SUB_I/F, MUL_I/F, DIV_I/F, MOD_I, NEG_I/F
  - Bitwise: BIT_AND, BIT_OR, BIT_XOR, BIT_NOT, SHL, SHR, USHR
  - Comparisons: EQ_I/F, NE_I/F, LT_I/F, LE_I/F, GT_I/F, GE_I/F (+ unsigned)
  - Control flow: JMP, JMP_IF, JMP_IF_NOT, RET, RET_VOID, RET_STRUCT_SMALL
  - Calls: CALL, CALL_NATIVE
  - Structs: GET_FIELD, SET_FIELD, STACK_ADDR, GET_FIELD_ADDR, STRUCT_LOAD_REGS, STRUCT_STORE_REGS, STRUCT_COPY
  - Arrays: GET_INDEX, SET_INDEX
- Constant pool for large values
- BCFunction and BCModule structures

### VM (`include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp`)

Virtual machine core:
- Shared register file with windowing for function calls
- Call frame stack for tracking active functions
- Module loading and function lookup

### Interpreter (`include/roxy/vm/interpreter.hpp`, `src/roxy/vm/interpreter.cpp`)

Switch-based interpreter loop:
- Handles 50+ opcodes
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

### Strings (`include/roxy/vm/string.hpp`, `src/roxy/vm/string.cpp`)

Heap-allocated string objects with full runtime support:
- **Memory layout**: `[ObjectHeader][StringHeader][char data + null]`
- **StringHeader**: Contains `length` and `capacity` fields
- **Escape sequences**: Parser handles `\n`, `\t`, `\r`, `\\`, `\"`, `\0`
- **Operations via native functions**: Concatenation, equality, length
- **Syntax rewriting**: IR builder rewrites `+` → `str_concat`, `==` → `str_eq`, `!=` → `str_ne`

Example string operations:
```
var s: string = "hello";
var greeting: string = "Hello, " + name + "!";
if (password == "secret") { ... }
var len: i32 = str_len(s);
print_str(greeting);
```

Key functions:
- `string_alloc(vm, data, length)` - Allocate new string, copies data
- `string_concat(vm, str1, str2)` - Concatenate two strings
- `string_equals(str1, str2)` - Compare strings for equality
- `string_length(data)` - Get string length
- `string_chars(data)` - Get pointer to character data

### Native Functions (`include/roxy/vm/natives.hpp`, `src/roxy/vm/natives.cpp`)

Built-in native function support:
- **Array functions:**
  - `array_new_int(size)` - Allocate integer array
  - `array_len(arr)` - Get array length
- **String functions:**
  - `str_concat(a, b)` - Concatenate two strings
  - `str_eq(a, b)` - String equality comparison
  - `str_ne(a, b)` - String inequality comparison
  - `str_len(s)` - Get string length
  - `print_str(s)` - Print string to stdout
- **Other:**
  - `print(value)` - Print integer value to stdout
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

**Struct parameters and returns** with automatic value/reference semantics:

| Struct Size | Slots | Passing Mode | Mechanism |
|-------------|-------|--------------|-----------|
| 1-8 bytes   | 1-2   | By value     | 1 register |
| 9-16 bytes  | 3-4   | By value     | 2 registers |
| >16 bytes   | >4    | By reference | Pointer (callee copies) |

- **Value semantics**: Callee always receives a copy - modifications don't affect caller
- **Small struct returns**: Returned in 1-2 registers, unpacked by caller
- **Large struct returns**: Caller passes hidden output pointer as last parameter, callee copies to it
- **Bytecode opcodes**: `STRUCT_LOAD_REGS`, `STRUCT_STORE_REGS`, `STRUCT_COPY`, `RET_STRUCT_SMALL`
- **SSA IR opcode**: `StructCopy` for memory-to-memory struct copies

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

Example struct parameter and return:
```
fun make_point(x: i32, y: i32): Point {
    return Point { x = x, y = y };
}
fun distance_sq(p: Point): i32 {
    return p.x * p.x + p.y * p.y;
}
```

Key types:
- `FieldInfo.slot_offset` / `FieldInfo.slot_count` - Field position in struct
- `StructTypeInfo.slot_count` - Total slots for struct
- `BCFunction.local_stack_slots` - Stack space needed per function
- `VarDecl.resolved_type` - Type resolved by semantic analysis
- `FieldInit` / `StructLiteralExpr` - AST nodes for struct literals

### Slab Allocator (`include/roxy/vm/slab_allocator.hpp`, `src/roxy/vm/slab_allocator.cpp`)

Custom slab allocator for heap objects with Vale-style random generational references:

- **Size classes**: 8 classes (32B to 4KB) plus large object support
- **Virtual memory**: Platform-specific `VirtualMemoryOps` for reserve/commit/decommit
- **Tombstoning**: Freed objects keep memory mapped, zeroed for safe weak ref checks
- **64-bit random generations**: xorshift128+ PRNG for weak reference validation
- **No generation reuse**: Random generations prevent wrap-around attacks

**ObjectHeader** (24 bytes):
```cpp
struct ObjectHeader {
    u64 weak_generation;    // 64-bit random generation
    u32 ref_count;          // Active ref borrows
    u32 type_id;            // Runtime type info
    u32 size;               // Total size including header
    u32 flags;              // FLAG_ALIVE or FLAG_TOMBSTONE
};
```

**Weak reference validation**:
```cpp
bool weak_ref_valid(void* data, u64 generation) {
    ObjectHeader* header = get_header_from_data(data);
    return header->is_alive() && (header->weak_generation == generation);
}
```

Key files:
- `include/roxy/vm/vmem.hpp` - Virtual memory interface
- `src/roxy/vm/vmem_win32.cpp` - Windows implementation (VirtualAlloc)
- `src/roxy/vm/vmem_unix.cpp` - Unix implementation (mmap)

## Partially Implemented (TODOs in code)

- **Method Lookup** - Semantic analysis has placeholder for proper method resolution

## Planned Components (Not Yet Implemented)

- LSP parser (error recovery, lossless CST)
- LSP server features (completion, hover, go-to-definition)
- Optimizations

## Testing

- **Framework:** doctest (vendored in `include/roxy/core/doctest/`)
- **Single executable:** `roxy_tests` contains all unit and E2E tests
- Use `TEST_CASE` and `SUBCASE` for test organization
- Use `CHECK` for assertions, `REQUIRE` for critical checks

### Test Helpers (`tests/e2e/test_helpers.hpp`)

The E2E tests use helper functions for compiling and running Roxy code:

```cpp
// Compile source to bytecode module
BCModule* compile(BumpAllocator& allocator, const char* source, bool debug = false);

// Compile and run, returning the result value
Value compile_and_run(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);

// Compile and run with stdout capture - preferred for E2E tests
TestResult run_and_capture(const char* source, StringView func_name, Span<Value> args = {}, bool debug = false);
```

**TestResult struct:**
```cpp
struct TestResult {
    i64 value;                    // Return value (always integer in Roxy)
    std::string stdout_output;    // Captured stdout
    bool success;                 // true if compilation and execution succeeded
};
```

**E2E test pattern:** Use `print()` and `print_str()` to output values, then verify with `stdout_output`:
```cpp
TEST_CASE("E2E - Example test") {
    const char* source = R"(
        fun main(): i32 {
            print(42);
            print_str("hello");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "42\nhello\n");
}
```

### Running Tests

```bash
cd build

# Run all tests
./roxy_tests

# Run only unit tests
./roxy_tests --test-case-exclude="E2E*"

# Run only E2E tests
./roxy_tests --test-case="E2E*"

# Run specific category
./roxy_tests --test-case="E2E - Struct*"

# List all test cases
./roxy_tests --list-test-cases
```

On Windows, use `.exe` extension.

## Documentation

- `CLAUDE.md` - This file, project guide for Claude Code
- `docs/overview.md` - Language design philosophy and roadmap
- `docs/grammar.md` - Formal grammar specification
- `docs/libraries.md` - Vendored library documentation
- `docs/internals/` - Detailed implementation documentation:
  - `vm.md` - VM state, interpreter loop, value representation
  - `bytecode.md` - Instruction encoding, opcode reference
  - `memory.md` - Reference types, object header, ref counting
  - `structs.md` - Stack-allocated structs, slot-based layout, struct parameters/returns
  - `arrays.md` - Dynamic arrays, bounds checking
  - `strings.md` - String objects, concatenation, comparison
  - `ssa-ir.md` - Block arguments, lowering to bytecode
  - `interop.md` - Native functions, automatic C++ binding
  - `frontend.md` - Lexer, parser, semantic analysis
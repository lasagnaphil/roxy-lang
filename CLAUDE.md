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
│   │   ├── lowering.hpp         # SSA to bytecode lowering
│   │   ├── module_registry.hpp  # Module registration and import resolution
│   │   └── compiler.hpp         # Multi-module compiler
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
│   │   ├── lowering.cpp         # Lowering implementation (536 lines)
│   │   ├── module_registry.cpp  # Module registry implementation
│   │   └── compiler.cpp         # Multi-module compiler implementation
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
│   └── e2e/                     # End-to-end tests (12 files)
│       ├── test_helpers.hpp     # Shared compile/run helpers
│       ├── test_helpers.cpp
│       ├── basics_test.cpp      # Variables, arithmetic, control flow
│       ├── recursion_test.cpp   # Recursive functions
│       ├── algorithms_test.cpp  # Complex algorithms, floats
│       ├── arrays_test.cpp      # Array operations, quicksort
│       ├── structs_test.cpp     # Struct fields, literals, params
│       ├── params_test.cpp      # Out/inout parameter tests
│       ├── interop_test.cpp     # C++ interop tests
│       ├── strings_test.cpp     # String operations, concatenation
│       ├── modules_test.cpp     # Module imports, multi-file compilation
│       ├── constructors_test.cpp # Named constructors/destructors
│       ├── methods_test.cpp     # Struct method calls
│       ├── inheritance_test.cpp # Struct inheritance, super calls
│       └── when_test.cpp        # When statement pattern matching
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
│       ├── frontend.md          # Lexer, parser, semantic analysis
│       └── modules.md           # Module system, imports, multi-file
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
- OOP: `self super new delete`
- References: `uniq ref weak out inout`
- Imports: `import from`

### Type Inference and Numeric Literals

Roxy supports local type inference with strict numeric typing:

**Variable declaration with inference:**
```roxy
var x = 42;        // Inferred as i32
var y = 3.14;      // Inferred as f64
var z = x + 10;    // Inferred as i32 (from expression type)
```

**Numeric literal types:**

| Literal | Type | Description |
|---------|------|-------------|
| `42` | `i32` | Default integer |
| `42u` | `u32` | Unsigned 32-bit |
| `42l` | `i64` | Signed 64-bit |
| `42ul` | `u64` | Unsigned 64-bit |
| `3.14` | `f64` | Default float |
| `3.14f` | `f32` | 32-bit float |

**Strict numeric typing:**
- No implicit conversions between numeric types
- Binary operators require matching types: `1 + 2l` is an error (i32 + i64)
- Use explicit suffixes to match types: `1l + 2l` works (i64 + i64)

### Primitive Type Casting

Explicit type conversions use constructor-like syntax:

```roxy
var x: i64 = 1000l;
var y: i32 = i32(x);      // Cast i64 to i32 (truncation)
var z: f64 = f64(42);     // Cast i32 to f64
var b: bool = bool(x);    // Cast integer to bool (0 = false, non-zero = true)
```

**Allowed conversions:**

| Source | Target | Behavior |
|--------|--------|----------|
| Any integer | Any integer | Truncation or sign/zero extension |
| Any integer | Any float | int-to-float conversion |
| Any float | Any integer | Truncation toward zero |
| f32 | f64 | Widening (lossless) |
| f64 | f32 | Narrowing (may lose precision) |
| Any integer/float | bool | Normalize: non-zero → true, zero → false |
| bool | Any integer/float | 0 or 1 |

**Disallowed:** `string` and `void` casts.

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
- 7 declaration types (variable, function, struct, enum, field, import, method)
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

### Enums

C-style enumerations with integer underlying type:

**Syntax:**
```roxy
enum Color { Red, Green, Blue }           // Auto-numbered: 0, 1, 2
enum Status { Pending = 0, Active = 10 }  // Explicit values
enum Code { A = 5, B, C }                 // B=6, C=7 (auto-increment)
```

**Usage:**
```roxy
var c: Color = Color::Red;
if (c == Color::Green) { ... }
```

**Implementation:**
- Enums compile to their underlying integer values (i32 by default)
- Variant access (`Type::Variant`) resolved via `ExprStaticGet` → `ConstInt`
- Enum variant symbols stored in global scope with their integer values
- Comparison uses standard integer comparison opcodes

**Files:** `types.hpp`, `ast.hpp`, `parser.cpp`, `semantic.cpp`, `ir_builder.cpp`
**Tests:** `tests/e2e/enums_test.cpp`

### When Statement (Pattern Matching)

The `when` statement provides pattern matching on enum values:

**Syntax:**
```roxy
when discriminant {
    case Variant1:
        // statements
    case Variant2, Variant3:
        // multiple variants
    else:
        // optional fallback
}
```

**Example:**
```roxy
enum Color { Red, Green, Blue }

fun describe(c: Color): i32 {
    when c {
        case Red:
            print_str("Red");
            return 1;
        case Green, Blue:
            print_str("Not red");
            return 2;
    }
    return 0;
}
```

**Features:**
- Discriminant must be an enum type
- Case names must be valid variants of the enum
- Multiple variants per case: `case A, B:`
- Optional `else:` clause for unhandled cases
- Duplicate case detection
- **Phi node support**: Variable modifications in case bodies persist after the `when` statement

**Implementation:**
- Generates comparison chain: `if (x == A) goto case_A else check_next`
- Each case body is a separate IR block
- Enum variant values looked up from symbol table
- Uses block parameters (phi nodes) to merge variable values from different branches
- Each case passes its current variable values as block arguments when jumping to merge block

**Example with variable modification:**
```roxy
enum Op { Add, Sub }

fun calc(op: Op, a: i32, b: i32): i32 {
    var result: i32 = a;
    when op {
        case Add:
            result = a + b;
        case Sub:
            result = a - b;
    }
    return result;  // Correctly returns modified value
}
```

**Files:** `ast.hpp`, `parser.cpp`, `semantic.cpp`, `ir_builder.cpp`
**Tests:** `tests/e2e/when_test.cpp`

### Tagged Unions (Discriminated Unions)

Tagged unions allow structs to contain variant-specific fields that share memory:

**Syntax:**
```roxy
enum Kind { A, B }

struct Data {
    common_field: i32;
    when kind: Kind {
        case A:
            val_a: i32;
        case B:
            val_b: f32;
    }
}
```

**Struct Literal with Variant:**
```roxy
var d: Data = Data { common_field = 1, kind = Kind::A, val_a = 42 };
```

**Pattern Matching on Tagged Unions:**
```roxy
fun process(d: ref Data) {
    when d.kind {
        case A:
            print(d.val_a);  // Variant field accessible in case
        case B:
            print_f32(d.val_b);
    }
}
```

**Memory Layout:**
- Fixed fields come first
- Discriminant field (enum value)
- Union storage (size = max of all variant sizes)
- All variants share the same memory region

**Type System:**
- `WhenClauseInfo`: Discriminant name, type, offsets, variants
- `VariantInfo`: Case name, discriminant value, variant fields
- `VariantFieldInfo`: Field info relative to union base offset

**Limitations (not yet implemented):**
- Flow-sensitive typing: Variant fields are accessible without being in a `when` case
- Exhaustiveness checking: No error if cases don't cover all variants
- Variant constructors: `Type.Variant { ... }` syntax not available

**Files:** `ast.hpp`, `types.hpp`, `parser.cpp`, `semantic.cpp`, `ir_builder.cpp`
**Tests:** `tests/e2e/tagged_unions_test.cpp`

### SSA IR (`include/roxy/compiler/ssa_ir.hpp`, `src/roxy/compiler/ssa_ir.cpp`)

SSA IR with block arguments (not phi nodes):
- 43 IR operations covering all basic operations:
  - Constants: ConstNull, ConstBool, ConstInt, ConstFloat, ConstString
  - Arithmetic: AddI/F, SubI/F, MulI/F, DivI/F, ModI, NegI/F
  - Comparisons: EqI/F, NeI/F, LtI/F, LeI/F, GtI/F, GeI/F
  - Logical: Not, And, Or; Bitwise: BitAnd, BitOr, BitNot
  - Memory: StackAlloc, GetField, GetFieldAddr, SetField, GetIndex, SetIndex
  - Structs: StructCopy, LoadPtr, StorePtr, VarAddr
  - Calls: Call, CallNative, CallExternal (cross-module)
- ValueId and BlockId for unique identification
- Block parameters for control flow merging
- Clean dataflow representation

### IR Builder (`include/roxy/compiler/ir_builder.hpp`, `src/roxy/compiler/ir_builder.cpp`)

Converts type-checked AST to SSA IR:
- Generates SSA form directly
- Handles control flow (if/else, while, for, when)
- Proper block argument insertion for loops and conditional branches
- Phi node support for variable modifications across branches (if/else, when)

### Bytecode (`include/roxy/vm/bytecode.hpp`, `src/roxy/vm/bytecode.cpp`)

32-bit fixed-width register-based bytecode:
- Three instruction formats: ABC (3-operand), ABI (immediate), AOFF (offset)
- 64 opcodes organized by category:
  - Constants/Moves: LOAD_NULL, LOAD_TRUE, LOAD_FALSE, LOAD_INT, LOAD_CONST, MOV
  - Arithmetic: ADD_I/F, SUB_I/F, MUL_I/F, DIV_I/F, MOD_I, NEG_I/F
  - Bitwise: BIT_AND, BIT_OR, BIT_XOR, BIT_NOT, SHL, SHR, USHR
  - Comparisons: EQ_I/F, NE_I/F, LT_I/F, LE_I/F, GT_I/F, GE_I/F (+ unsigned)
  - Type conversions: I_TO_F64, F64_TO_I, I_TO_B, B_TO_I, TRUNC_S, TRUNC_U, F32_TO_F64, F64_TO_F32, I_TO_F32, F32_TO_I
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

### Methods (`types.hpp`, `ast.hpp`, `parser.cpp`, `semantic.cpp`, `ir_builder.cpp`)

External method declarations for structs using `fun StructName.method()` syntax:

**Declaration syntax:**
```roxy
struct Point {
    x: i32;
    y: i32;
}

fun Point.sum(): i32 {
    return self.x + self.y;
}

fun Point.add(dx: i32, dy: i32): i32 {
    return self.x + dx + self.y + dy;
}
```

**Method calls:**
```roxy
fun main(): i32 {
    var p: Point = Point { x = 10, y = 20 };
    print(p.sum());       // Prints 30
    print(p.add(5, 15));  // Prints 50
    return 0;
}
```

**Key Implementation Details:**

- **Name mangling**: Methods are compiled as regular functions with mangled names: `StructName$method_name`
- **Implicit `self`**: First parameter is always `self` (type `ref<StructType>`), automatically passed
- **Storage**: `StructTypeInfo.methods` stores `Span<MethodInfo>` with method metadata
- **AST**: `DeclMethod` kind with `MethodDecl` struct containing struct_name, name, params, return_type, body
- **Semantic**: Multi-pass analysis - register methods in type resolution, analyze bodies later
- **IR**: `build_method()` generates IR with `self` as first parameter

**Method Features:**
- Methods can read and modify `self` fields
- Methods can take additional parameters
- Methods can return any type including structs
- Works with both stack-allocated and heap-allocated (`uniq`) structs
- Multiple methods can be defined on the same struct

**MethodInfo struct:**
```cpp
struct MethodInfo {
    StringView name;
    Span<Type*> param_types;  // NOT including implicit self
    Type* return_type;
    Decl* decl;               // Points to MethodDecl AST node
};
```

Example with mutation:
```roxy
struct Counter {
    value: i32;
}

fun Counter.increment() {
    self.value = self.value + 1;
}

fun main(): i32 {
    var c: Counter = Counter { value = 0 };
    c.increment();
    print(c.value);  // Prints 1
    return 0;
}
```

### Struct Inheritance (`types.hpp`, `semantic.cpp`, `ir_builder.cpp`)

Single inheritance for structs with static dispatch (no vtables):

**Syntax:**
```roxy
struct Animal {
    hp: i32;
}

fun Animal.speak(): i32 {
    return 1;
}

struct Dog : Animal {
    breed: i32;
}

fun Dog.speak(): i32 {
    return super.speak() + 10;  // Calls parent method
}

fun new Dog(hp: i32, breed: i32) {
    super(hp);        // Call parent constructor (or implicit if omitted)
    self.breed = breed;
}
```

**Key Features:**

| Feature | Description |
|---------|-------------|
| Field inheritance | Child inherits all parent fields, laid out parent-first |
| Method inheritance | Child can call inherited methods, override with same name |
| `super.method()` | Call parent's version of a method |
| `super()` / `super(args)` | Call parent constructor (implicit if omitted) |
| Constructor chaining | Parent constructor runs before child |
| Destructor chaining | Child destructor runs before parent (automatic) |
| Value slicing | Assigning Child to Parent copies only parent fields |
| Covariant references | `uniq<Child>` assignable to `ref<Parent>` |

**Implementation:**

- **Type system**: `StructTypeInfo.parent` stores parent type pointer
- **Subtyping**: `is_subtype_of()` walks parent chain for compatibility checks
- **Method lookup**: `lookup_method_in_hierarchy()` searches child-to-parent
- **Name mangling**: Methods as `StructName$$method_name`, constructors as `StructName$$new`
- **Static dispatch**: Method calls resolved at compile time based on declared type

**Example multi-level inheritance:**
```roxy
struct Animal { hp: i32; }
struct Dog : Animal { breed: i32; }
struct Labrador : Dog { color: i32; }

fun main(): i32 {
    var lab: Labrador = Labrador { hp = 100, breed = 5, color = 3 };
    print(lab.hp);     // 100 - inherited from Animal
    print(lab.breed);  // 5 - inherited from Dog
    print(lab.color);  // 3 - own field
    return 0;
}
```

### Slab Allocator (`include/roxy/vm/slab_allocator.hpp`, `src/roxy/vm/slab_allocator.cpp`)

Custom slab allocator for heap objects with Vale-style random generational references:

- **Size classes**: 8 classes (32B to 4KB) plus large object support
- **Virtual memory**: Platform-specific `VirtualMemoryOps` for reserve/commit/decommit
- **Tombstoning**: Freed objects keep memory mapped, zeroed for safe weak ref checks
- **Slab reclamation**: `reclaim_tombstoned()` releases physical memory from fully tombstoned slabs
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

### Module System (`include/roxy/compiler/module_registry.hpp`, `include/roxy/compiler/compiler.hpp`)

Multi-file compilation with import/export support:

**Import Syntax:**
```roxy
import math;                    // Import module, access as math.sin()
import math.vec2;               // Nested path, access as vec2.add()
from math import sin, cos;      // Import specific symbols directly
from math.vec2 import add;      // From-import with nested path
from utils import clamp as c;   // Import with alias
```

**Nested Module Paths:**
- Dotted paths like `math.vec2` are supported for organizing modules hierarchically
- The **last component** of the path becomes the local name: `import math.vec2;` → access as `vec2.func()`
- Module must be registered with the full path: `compiler.add_source("math.vec2", source, len)`
- Works with both `import` and `from ... import` forms
- No namespace hierarchy at runtime - just convenient naming

**Key Components:**

| Component | Description |
|-----------|-------------|
| `ModuleRegistry` | Tracks all modules (native and script) and their exports |
| `ModuleInfo` | Module metadata: name, exports list, native registry pointer |
| `ModuleExport` | Export entry: name, kind (Function/Struct/Enum), type, visibility |
| `NativeRegistry` | C++ function binding (separate from ModuleRegistry) |
| `Compiler` | Multi-module compiler with topological sorting |

**Builtin Prelude:**
- Built-in functions (`print`, `str_concat`, `array_new_int`, etc.) are registered as a `"builtin"` module
- The builtin module is **auto-imported** as a prelude during semantic analysis
- No explicit import needed for built-in functions

**Multi-Module Compilation:**
```cpp
Compiler compiler(allocator);
compiler.add_source("utils", utils_source, utils_len);
compiler.add_source("main", main_source, main_len);
BCModule* module = compiler.compile();  // Links all modules together
```

**Compilation Pipeline:**
1. Parse all modules
2. Topologically sort by import dependencies (detect cycles)
3. Semantic analysis in dependency order
4. Build IR for all modules
5. Link into single BCModule

**Cross-Module Calls (Static Linking):**
- `IROp::CallExternal` - IR instruction for cross-module function calls
- At link time, all modules are merged into a single IR module
- Cross-module calls are resolved to regular `CALL` instructions (no runtime resolution)

**Visibility:**
- Functions marked `pub` are exported and can be imported by other modules
- Non-public functions are module-private

Key files:
- `include/roxy/compiler/module_registry.hpp` - ModuleInfo, ModuleExport, ModuleRegistry
- `src/roxy/compiler/module_registry.cpp` - Module registration implementation
- `include/roxy/compiler/compiler.hpp` - Compiler class for multi-module compilation
- `src/roxy/compiler/compiler.cpp` - Topological sort, linking, cycle detection
- `include/roxy/vm/natives.hpp` - BUILTIN_MODULE_NAME constant

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

    TestResult result = run_and_capture(source, "main");
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
  - `modules.md` - Module system, imports, multi-file compilation
  - `constructors.md` - Named constructors/destructors, `self` keyword, synthesized defaults
  - `methods.md` - Struct methods, `self` parameter, name mangling
  - `inheritance.md` - Struct inheritance, subtyping, `super` keyword, constructor/destructor chaining
  - `tagged-unions.md` - Discriminated unions with `when` clause, variant fields, union layout
  - `generics.md` - Generic functions and structs (NOT YET IMPLEMENTED - design document)
  - `traits.md` - Traits and trait bounds (NOT YET IMPLEMENTED - design document)
  - `operator-overloading.md` - Operator traits for custom operators (NOT YET IMPLEMENTED - design document)
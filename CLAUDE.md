# CLAUDE.md - Roxy Language Codebase Guide

## Overview

Roxy is an embeddable statically-typed scripting language written in C++20. It is designed as a fast, modern alternative to Lua with:
- Static typing by default for better IDE support and optimizations
- Value semantics to reduce boxing overhead
- Smart pointer system (unique/strong/weak references) instead of garbage collection
- C/C++ interoperability
- Fast bytecode compilation and execution

## Build System

**Requirements:**
- CMake 3.26+
- C++20 compatible compiler

**Build Commands:**
```bash
mkdir build && cd build
cmake ..
cmake --build .
```

**Targets:**
- `roxy` - Static library containing the compiler and VM
- `roxy_bin` - CLI executable

**Key Definitions:**
- `FMT_EXCEPTIONS=0` - fmt library without exceptions
- `XXH_INLINE_ALL` - xxHash inlined for performance

## Project Structure

```
roxy-lang/
├── include/roxy/           # Public headers
│   ├── core/               # Core utilities (allocator, vectors, types)
│   ├── scanner.hpp         # Lexical analyzer
│   ├── parser.hpp          # Recursive descent parser
│   ├── expr.hpp            # Expression AST nodes
│   ├── stmt.hpp            # Statement AST nodes
│   ├── type.hpp            # Type system
│   ├── sema.hpp            # Semantic analysis
│   ├── opcode.hpp          # Bytecode instructions
│   ├── chunk.hpp           # Bytecode chunks
│   ├── vm.hpp              # Virtual machine
│   ├── module.hpp          # Module representation
│   ├── library.hpp         # Library manager
│   ├── value.hpp           # Runtime values
│   ├── object.hpp          # Heap objects (ref counting)
│   └── string_interner.hpp # String interning
├── src/roxy/               # Implementation files
│   └── roxy-bin/main.cpp   # CLI entry point
├── docs/                   # Documentation
│   ├── overview.md         # Language overview & features
│   ├── grammar.md          # Formal grammar
│   ├── internals.md        # VM & calling conventions
│   └── libraries.md        # Embedded libraries
└── examples/               # Example programs & tests
    └── test/               # Test suite (400+ files)
```

## Compilation Pipeline

```
Source Code → Scanner → Parser → Semantic Analysis → Code Generation → VM Execution
```

### 1. Scanner (`scanner.hpp/cpp`)
- Produces tokens from source code
- Supports keywords: `struct, fun, var, if, else, for, while, return, break, continue, import, native, pub`
- Line tracking for error reporting
- String interning support

### 2. Parser (`parser.hpp/cpp`)
- Recursive descent with precedence climbing
- Uses bump allocator (`AstAllocator`) for efficient AST allocation
- Precedence levels: Assignment > Ternary > Or > And > Equality > Comparison > Term > Factor > Unary > Call > Primary

### 3. AST Nodes
**Expressions (`expr.hpp`):** Error, Assign, Binary, Ternary, Grouping, Literal, Unary, Variable, Call, Get, Set

**Statements (`stmt.hpp`):** Error, Block, Module, Expression, Struct, Function, If, Var, While, Return, Break, Continue, Import

### 4. Type System (`type.hpp`)
- `PrimitiveType`: bool, i8-i64, u8-u64, f32, f64, string, void
- `StructType`: User-defined structures
- `FunctionType`: Function signatures

### 5. Semantic Analysis (`sema.hpp/cpp`)
- Type checking and inference
- Variable resolution
- Function parameter validation
- Struct field validation
- Uses `SemaEnv` for scoped symbol lookup with robin_map

### 6. Code Generation → Bytecode (`opcode.hpp`, `chunk.hpp`)
**Key opcodes:**
- Load/Store: `iload`, `lload`, `rload` (int32, int64, reference)
- Constants: `iconst`, `lconst`, `fconst`, `dconst`
- Arithmetic: `iadd/isub/imul/idiv`, `ladd/lsub/...`, `fadd/fsub/...`, `dadd/dsub/...`
- Control flow: `jmp`, `loop`, `br_false_s`, `br_icmpeq`, etc.
- Functions: `call`, `callnative`, `ret`, `iret`, `lret`
- Strings: `ldstr`

### 7. Virtual Machine (`vm.hpp/cpp`)
- Stack-based interpreter
- Max 64 call frames
- 16K stack slots (64 * 256)
- Arguments pushed in order, reused as local variables
- Return values left on stack

## Key Design Patterns

### Bump Allocator (`core/bump_allocator.hpp`)
Used for AST allocation - allocations never freed during compilation, efficient for compile-time structures.

### Relative Pointers (`core/rel_ptr.hpp`)
Compact pointer representation using offsets, works when data is moved/relocated.

### Reference Counting (`object.hpp`)
- Intrusive reference counting for heap objects
- Random generational UIDs for weak pointer validation
- Prevents use-after-free in weak references

### String Interning (`string_interner.hpp`)
All strings interned in a hash table for O(1) comparison and memory deduplication.

## Language Syntax

### Variables
```roxy
var x: int = 42;
var y: float = 3.14f;
var b: bool = true;
var s: string = "hello";
```

### Structures (with inheritance)
```roxy
struct Point {
    x: double
    y: double
}

struct Entity {
    hp: int
}

struct Player : Entity {  // Inheritance
    mp: int
}

var p: Point;
p.x = 10.0;
```

### Functions
```roxy
fun add(a: int, b: int): int {
    return a + b;
}

fun fib(n: int): int {
    if (n <= 1) return n;
    return fib(n - 1) + fib(n - 2);
}
```

### Named Constructors/Destructors
```roxy
fun new Player.default_character() {  // Custom constructor
    super.new(hp = 100);
    mp = 50;
}

fun delete Player.remove_from_game() {  // Custom destructor
    print("Cleanup!");
}

// Usage:
var player = uniq new Player.default_character();
delete player.remove_from_game();
```

### Control Flow
```roxy
if (x > 0) { ... } else { ... }
while (x > 0) { x = x - 1; }
for (var i = 0; i < 10; i = i + 1) { ... }
```

### Enums and Tagged Unions
```roxy
enum SkillType {
    Attack, Defend
}

struct Skill {
    name: string
    when type: SkillType {  // Tagged union
        case Attack:
            damage: int
        case Defend:
            armor: int
    }
}
```

### Operators
- Arithmetic: `+, -, *, /, %`
- Comparison: `==, !=, <, <=, >, >=`
- Logical: `&&, ||, !`
- Bitwise: `&, |, ^, <<, >>, ~`
- Ternary: `condition ? true_expr : false_expr`

## Reference Types

Roxy uses smart pointers instead of garbage collection:

| Type | Description |
|------|-------------|
| `uniq` | Unique reference - owns the object, can be new/deleted |
| `ref` | Strong reference - ref-counted, asserts on dangling |
| `weak` | Weak reference - uses generational UIDs, can be null |
| `inout` | Read-write reference to caller's value (like C# `ref`) |
| `out` | Write-only reference (like C# `out`) |

**Auto-conversion rules:**
- `value` → `uniq`, `value` → `inout`, `value` → `out`
- `uniq` → `ref`, `uniq` → `weak`
- `ref` → `weak`
- `inout` → `out`

**Note:** There is no `in` reference - just pass by value. Values are copied only if modified inside the function or small enough (≤16 bytes).

## Calling Convention

- Arguments pushed to stack, reused as local variable storage
- Pass-by-value: parameters copied to stack
- `ref` parameters: pointer to caller's stack variable is pushed
- Struct layout follows C ABI (fields aligned)

## Testing

Test files located in `examples/test/` with 400+ test cases covering:
- Assignment, basic types, control flow
- Functions, recursion, closures
- Operators (arithmetic, comparison, logical, bitwise)
- Scoping, variable shadowing
- Benchmarks (fibonacci, trees)

## Key Source Files

| File | Purpose |
|------|---------|
| `scanner.cpp` | Tokenization |
| `parser.cpp` | AST construction |
| `sema.cpp` | Type checking, symbol resolution |
| `vm.cpp` | Bytecode execution |
| `object.cpp` | Reference counting, heap objects |
| `string_interner.cpp` | String deduplication |
| `library.cpp` | Module and library management |
| `roxy-bin/main.cpp` | CLI entry point |

## Embedded Libraries

These libraries are directly embedded in the roxy library:

| Library | Version | Purpose |
|---------|---------|---------|
| fmt | 10.1.1 | String formatting and internal logging (exceptions disabled) |
| xxHash | 0.8.2 | Fast XXH3 hash functions (64-bit, inlined) |
| tsl robin-map | 1.2.1 | Fast hash table for string interning |

## Design Goals

- **IDE support baked in**: Compiler built from day 1 for LSP server support (like Roslyn)
- **Fast incremental compilation**: Smart caching system that reuses AST fragments
- **Embeddability**: Clean C++ API, minimal runtime overhead, no exceptions
- **C/C++ struct interop**: Expose POD structs directly without boxing (8-byte aligned fields)

## Current Development Status

Recent work includes:
- Runtime struct support implementation
- Semantic analysis refactoring
- Type resolution for function arguments
- Function call bug fixes

**Known TODOs:**
- Module system linking
- Array/table subscript syntax
- Method calls on objects
- Full inheritance implementation
- REPL support
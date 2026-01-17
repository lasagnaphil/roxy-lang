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
- **Compiler:** clang-cl (Windows)
- **C++ Standard:** C++17

### Build Commands

```bash
cd build
cmake .. -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl -DCMAKE_MT="C:/Program Files/LLVM/bin/llvm-mt.exe"
ninja
```

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
│   ├── core/           # Core utilities (types, containers)
│   │   ├── types.hpp   # Type aliases (u32, i64, f64, etc.)
│   │   ├── span.hpp    # Non-owning array view
│   │   ├── vector.hpp  # Dynamic array
│   │   └── ...
│   ├── shared/         # Shared frontend components
│   │   ├── token_kinds.hpp  # TokenKind enum
│   │   ├── token.hpp        # Token, SourceLocation structs
│   │   └── lexer.hpp        # Lexer class
│   └── ...
├── src/roxy/
│   ├── core/
│   │   └── file.cpp
│   └── shared/
│       ├── token_kinds.cpp  # token_kind_to_string()
│       └── lexer.cpp        # Lexer implementation
├── tests/
│   └── lexer_test.cpp
├── docs/
│   ├── overview.md     # Language features and design
│   └── grammar.md      # Roxy grammar specification
├── DESIGN.md           # Detailed VM and compiler design
└── CMakeLists.txt
```

## Compiler Pipeline

```
Source → Lexer → Parser → AST → SSA IR (block arguments) → Register Bytecode → VM
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

The lexer tokenizes Roxy source code. Features:
- Decimal, hex (`0xFF`), binary (`0b1010`), octal (`0o77`) number literals
- Integer suffixes (`u`, `l`, `ul`) and float suffix (`f`)
- String literals with escape sequences (`\n`, `\t`, `\\`, `\"`)
- All operators including two-character ones (`::`, `&&`, `||`, `+=`, etc.)
- Line comments (`//`) and nested block comments (`/* */`)
- Keyword recognition via trie-style switch (O(1) for non-keywords)
- Accurate line/column tracking

## Planned Components (Not Yet Implemented)

- Compiler parser (fail-fast)
- LSP parser (error-recovering)
- SSA IR with block arguments
- Register-based bytecode VM
- C++ interop layer

## Testing

Run tests after building:
```bash
./lexer_test.exe
```

## Documentation

- `docs/overview.md` - Language design and features
- `docs/grammar.md` - Formal grammar specification
- `DESIGN.md` - Detailed implementation design (VM, bytecode, interop)

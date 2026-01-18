# Roxy Language Design

Roxy is an embeddable scripting language for game engines with:
- Static typing
- Value semantics by default
- Memory management via `uniq`/`ref`/`weak` references (no GC)
- Fast C++ interop
- Future AOT compilation to C

## Compiler Pipeline

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Lowering → Bytecode → VM
```

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        Frontend                                  │
├─────────────────────────────────────────────────────────────────┤
│  Lexer → Parser → AST → Semantic Analysis → Symbol Table        │
│                                                                  │
│  See: docs/internals/frontend.md                                │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                     SSA IR Generation                            │
├─────────────────────────────────────────────────────────────────┤
│  IR Builder → SSA IR (block arguments) → Lowering → Bytecode    │
│                                                                  │
│  See: docs/internals/ssa-ir.md                                  │
└─────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────┐
│                      Virtual Machine                             │
├─────────────────────────────────────────────────────────────────┤
│  Bytecode Interpreter ← Register File ← Local Stack             │
│                                                                  │
│  See: docs/internals/vm.md, docs/internals/bytecode.md          │
└─────────────────────────────────────────────────────────────────┘
```

## Detailed Documentation

| Topic | Document | Description |
|-------|----------|-------------|
| Virtual Machine | [docs/internals/vm.md](docs/internals/vm.md) | VM state, interpreter loop, value representation |
| Bytecode | [docs/internals/bytecode.md](docs/internals/bytecode.md) | Instruction encoding, opcode reference |
| Memory Model | [docs/internals/memory.md](docs/internals/memory.md) | Reference types, object header, ref counting |
| Structs | [docs/internals/structs.md](docs/internals/structs.md) | Stack-allocated structs, slot-based layout |
| Arrays | [docs/internals/arrays.md](docs/internals/arrays.md) | Dynamic arrays, bounds checking |
| SSA IR | [docs/internals/ssa-ir.md](docs/internals/ssa-ir.md) | Block arguments, lowering to bytecode |
| C++ Interop | [docs/internals/interop.md](docs/internals/interop.md) | Native functions, automatic binding |
| Frontend | [docs/internals/frontend.md](docs/internals/frontend.md) | Lexer, parser, semantic analysis |

## Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| IR representation | SSA with block arguments | Cleaner than phi nodes, easier lowering |
| Bytecode format | 32-bit fixed-width, register-based | Easy C transpilation, natural SSA lowering |
| Register file | Untyped `u64` slots | Performance, type info only for debug |
| Struct storage | Separate local stack (`u32` slots) | 4-byte granularity, C++ interop alignment |
| Memory management | Reference counting (`uniq`/`ref`/`weak`) | Predictable, no GC pauses |
| Parser strategy | Separate compiler/LSP parsers | Fail-fast compile, error-recovery for LSP |

## Implementation Status

| Component | Status |
|-----------|--------|
| Lexer | ✅ Done |
| Parser | ✅ Done |
| AST | ✅ Done |
| Type System | ✅ Done |
| Semantic Analysis | ✅ Done |
| SSA IR | ✅ Done |
| IR Builder | ✅ Done |
| Bytecode | ✅ Done |
| VM/Interpreter | ✅ Done |
| Lowering | ✅ Done |
| Arrays | ✅ Done |
| Native Functions | ✅ Done |
| C++ Interop | ✅ Done |
| Structs | ✅ Done |
| LSP Parser | ⏳ Planned |
| LSP Features | ⏳ Planned |

## Next Steps

1. ~~Lexer~~ ✅
2. ~~Compiler parser~~ ✅
3. ~~SSA IR~~ ✅
4. ~~Bytecode lowering~~ ✅
5. ~~VM interpreter~~ ✅
6. ~~Arrays~~ ✅
7. ~~C++ interop~~ ✅
8. ~~Structs~~ ✅
9. Struct extensions (literals, pass-by-value, return, nested)
10. Heap allocation (`new`/`uniq`/`ref`/`weak` for structs)
11. LSP parser (error recovery, lossless CST)
12. LSP features (completion, hover, go-to-definition)

## Test Coverage

All components have test coverage:

```
tests/
├── lexer_test.cpp      # Token scanning
├── parser_test.cpp     # AST construction
├── semantic_test.cpp   # Type checking
├── ssa_ir_test.cpp     # IR generation
├── bytecode_test.cpp   # Instruction encoding
├── vm_test.cpp         # VM execution
├── lowering_test.cpp   # SSA to bytecode
├── e2e_test.cpp        # End-to-end tests
└── interop_test.cpp    # C++ interop
```

Run all tests:
```bash
cd build
./lexer_test && ./parser_test && ./semantic_test && \
./ssa_ir_test && ./bytecode_test && ./vm_test && \
./lowering_test && ./e2e_test && ./interop_test
```

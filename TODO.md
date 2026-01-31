# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-01-31

---

## Critical (Blocking Features)

- [ ] **Method calls not implemented**
  - File: `src/roxy/compiler/semantic.cpp:1635`
  - Issue: `analyze_super_expr()` errors with "Method calls are not yet implemented"
  - Impact: Blocks inheritance-based programming (`super.method()` calls)
  - Scope: AST supports `SuperExpr` and `StructDecl.methods` but semantic analysis lacks method lookup
  - Related: Struct inheritance is parsed but methods cannot be called

- [ ] **When/case statements not implemented**
  - Files: Parser, Semantic, IR Builder
  - Issue: `KwWhen` and `KwCase` tokens are lexed but no AST nodes or parsing exist
  - Missing: `StmtWhen`, `StmtCase` in AstKind, no `when_statement()` in parser
  - Impact: Blocks enum pattern matching shown in `docs/overview.md`
  - Example: `when skill.type { case Attack: ... }` won't parse

- [ ] **Cross-module function execution incomplete**
  - File: `src/roxy/vm/interpreter.cpp:460`
  - Issue: `CALL_EXTERNAL` opcode raises "External function not resolved - multi-module support not yet implemented"
  - Status: ModuleRegistry and import/export work, but VM can't execute cross-module calls
  - Impact: Multi-file programs with function calls between modules won't run

---

## High Priority

- [ ] **Register overflow assertion**
  - File: `src/roxy/compiler/lowering.cpp:208`
  - Issue: `assert(m_next_reg < 255)` - functions with >255 SSA values crash
  - Fix: Return error or implement register spilling

- [ ] **Value allocation assertion**
  - File: `src/roxy/compiler/lowering.cpp:218`
  - Issue: Unallocated values crash with assert
  - Fix: Should be caught in IR validation pass, not at lowering time

- [ ] **Reference count assertion in release builds**
  - File: `src/roxy/vm/object.cpp:81`
  - Issue: `ref_dec()` asserts `ref_count > 0` - release builds won't catch invalid decrement
  - Fix: Validate and return error instead of asserting

- [ ] **Global static type IDs not thread-safe**
  - Files: `src/roxy/vm/array.cpp:7`, `src/roxy/vm/string.cpp:9`
  - Issue: `g_array_type_id` and `g_string_type_id` use static globals with lazy initialization
  - Fix: Move type registration to per-VM or use proper synchronization

---

## Medium Priority

- [ ] **Multi-register struct argument tracking**
  - File: `src/roxy/compiler/lowering.cpp:413`
  - Issue: TODO comment - struct arguments spanning multiple registers not fully tracked
  - Fix: Complete register tracking for multi-register struct params

- [ ] **Magic numbers for 16-bit range**
  - Files: `src/roxy/compiler/lowering.cpp:235,287`
  - Fix: Extract `-32768`/`32767` to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:28`
  - Issue: `if (size > 1000000)` - arbitrary undocumented limit
  - Fix: Make configurable via VMConfig or define named constant

- [ ] **Float division behavior inconsistency**
  - File: `src/roxy/vm/interpreter.cpp:110-147`
  - Issue: Float div-by-zero produces IEEE 754 infinity/NaN, but integer div-by-zero errors
  - Fix: Document behavior or add optional strict mode

- [ ] **Local function declarations explicitly blocked**
  - File: `src/roxy/compiler/semantic.cpp:886`
  - Issue: Grammar allows local functions but semantic analysis rejects them
  - Decision needed: Support or remove from grammar

---

## Low Priority

- [ ] **Error handling standardization**
  - Issue: Mixed patterns across compiler - Result type vs error_type vs nullptr
  - Files: semantic.cpp, ir_builder.cpp, lowering.cpp
  - Fix: Standardize on single error handling pattern

- [ ] **Add IR validation pass**
  - Issue: Invalid IR can reach lowering and cause cryptic errors
  - Fix: Add validation pass between IR builder and lowering

- [ ] **Audit emit_inst() call sites**
  - File: `src/roxy/compiler/lowering.cpp`
  - Issue: Not all call sites check for nullptr returns
  - Fix: Add consistent error checking

---

## Planned Features

- [ ] Method calls and inheritance (requires type system extension for method lookup)
- [ ] When/case statements for enum pattern matching
- [ ] Function overloading
- [ ] Operator overloading
- [ ] Exception handling
- [ ] LSP server for IDE support
- [ ] AOT compilation to C

---

## Code Quality Improvements

- [ ] Extract bytecode constants to dedicated header
- [ ] Add inline comments explaining bytecode instruction formats
- [ ] Standardize error message formatting across compiler
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)
- [ ] Document register limit (255 values per function)
- [ ] Document when/case syntax once implemented
- [ ] Add bytecode instruction format reference

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access (>5 levels)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs
- [ ] Test struct inheritance when implemented

---

## Recently Completed

- [x] Remove `new` keyword from constructor calls (2026-01-31)
  - Constructor calls now use `Type()` instead of `new Type()`
  - Heap allocation uses `uniq Type()` instead of `uniq new Type()`
  - `new` keyword kept for constructor declarations (`fun new Type()`)

- [x] Named constructors/destructors with `self` keyword (2026-01-31)

- [x] Module system with import/export (compile-time, runtime pending)

- [x] Struct field visibility checking

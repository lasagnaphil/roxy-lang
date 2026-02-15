# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-02-16

---

## High Priority

*(No items — all resolved)*

---

## Medium Priority

- [ ] **Magic numbers for 16-bit range**
  - Files: `src/roxy/compiler/lowering.cpp:235,287`
  - Fix: Extract `-32768`/`32767` to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:23`
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

- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
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
- [ ] Add bytecode instruction format reference

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access (>5 levels)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs

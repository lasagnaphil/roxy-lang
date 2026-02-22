# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-02-22

---

## High Priority

*(No items)*

---

## Medium Priority

- [ ] **Covariant mutable reference subtyping is unsound**
  - File: `src/roxy/compiler/semantic.cpp` (`check_assignable`)
  - Issue: `ref<Child>` is assignable to `ref<Parent>`, but `ref` is mutable — allows writing a `Parent` value through a reference that actually points to a `Child`, corrupting layout
  - Fix: Make reference subtyping invariant, or split into read-only `ref` (covariant) and mutable `mut_ref` (invariant)

---

## Low Priority

- [ ] **Error handling standardization**
  - Issue: Mixed patterns across compiler - Result type vs error_type vs nullptr
  - Files: semantic.cpp, ir_builder.cpp, lowering.cpp
  - Fix: Standardize on single error handling pattern

- [ ] **Add IR validation pass**
  - Issue: Invalid IR can reach lowering and cause cryptic errors
  - Fix: Add validation pass between IR builder and lowering

---

## Planned Features

- [ ] Bounded quantification Phase B: declaration-site checking of generic bodies against trait bounds
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] Exception handling
- [ ] LSP server for IDE support
- [ ] AOT compilation to C (design plan complete in `docs/internals/c-backend.md`)

---

## Code Quality Improvements

- [ ] Standardize error message formatting across compiler
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)

---

## Testing Gaps

- [ ] Test deeply nested struct field access (>5 levels; currently only 3 levels tested)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs (diamond dependencies, >3 levels)


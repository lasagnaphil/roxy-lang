# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-04-25


---

## High Priority

(none currently)

---

## Medium Priority

(none currently)

---

## Low Priority

(none currently)

---

## Planned Features

- [ ] Closures and first-class functions — functions as values, lambda syntax, closure environment capture, `fun(params): ret` function type syntax
- [ ] Bounded quantification Phase B: declaration-site checking of generic bodies against trait bounds
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration)
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)
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
- [ ] Test variable shadowing with noncopyable types (inner scope same name as moved outer variable)
- [ ] Test struct literals with noncopyable field values (source variable should be marked moved)
- [ ] Test self-assignment of noncopyable variables (`x = x`)

# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-04-12

---

## High Priority

*(No items)*

---

## Medium Priority

- [x] Destructor throw during exception unwinding loses original exception — fixed: compile-time ban on `throw` in delete destructors + runtime safety net via `in_flight_exception` guard
- [ ] Ternary expressions skip move-state merging — unlike `if`/`else`, ternary expressions don't save/restore/merge move states, so conditional moves in ternary branches may not be detected (`semantic.cpp:3685-3728`)
- [ ] `alloc_large()` only zeros `size` bytes, not page-aligned `alloc_size` bytes — padding between `size` and `alloc_size` contains uninitialized memory, unlike slab path which zeros the full slot (`slab_allocator.cpp:229`)
- [ ] Self-assignment of noncopyable values causes use-after-free — `x = x` for noncopyable types: IR emits auto-delete of old value then assigns the now-dangling pointer; semantic analyzer should reject this case (`semantic.cpp:4883-4980`)
- [ ] No definite-termination analysis for move-state merging — if one branch always returns/throws, the merge still produces MaybeValid instead of taking the surviving branch's state, rejecting valid code like `if (err) { return; } consume(x);` (`semantic.cpp:2970-2998`)

---

## Low Priority

- [ ] `find_slab_containing()` O(N) linear scan on every `free()` — consider sorted data structure or embedding slab identity in object header (`slab_allocator.cpp:248-276`)
- [ ] Tombstoned slab slots are never recycled to the free list — mixed-lifetime allocation patterns cause permanent fragmentation within size classes until full-slab reclamation
- [ ] Nested field moves rejected for value-type chains — `check_not_field_move` only handles single-level `obj.field`, rejects `obj.inner.field` even when all types are value types (`semantic.cpp:1451`)
- [ ] For-loop increment analyzed before body — increment expression moves are visible in body, but at runtime increment runs after body; minor false positive on first iteration (`semantic.cpp:3068-3069`)

---

## Planned Features

- [ ] Closures and first-class functions — functions as values, lambda syntax, closure environment capture, `fun(params): ret` function type syntax
- [ ] Recursive types — allow `uniq`/`ref` members to the same type or boxed/indirect fields for tree/graph data structures
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


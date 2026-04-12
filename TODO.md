# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-03-08

---

## High Priority

- [x] Large object virtual address space leak — `free_large()` calls `remap_to_zero()` then erases from `large_objects` map, making the virtual address range permanently untracked with no reclamation path (`slab_allocator.cpp:294-311`)
- [x] Noncopyable container elements leaked on exception unwind — cleanup kinds 3/4 (LIST_CLEANUP/MAP_CLEANUP) call `object_free()` on the container but do not iterate and destroy noncopyable elements (`interpreter.cpp:142-146`)
- [ ] Temporary uniq values not tracked for exception cleanup — intermediate `uniq` values in expressions are not added to `m_owned_locals` and have no `BCCleanupRecord`, so they leak if an exception is thrown mid-expression
- [ ] Variable shadowing corrupts move states — `m_move_states` is keyed by bare `StringView` name; inner-scope declaration of same name overwrites outer variable's Moved state to Live, allowing use-after-move after inner scope exits (`semantic.cpp:1399-1401`)
- [ ] Struct literal fields don't mark source variables as moved — `analyze_struct_literal_expr` type-checks noncopyable field values but never calls `mark_moved` or `check_not_field_move`, so `Wrapper { item = x }` leaves x as Live (`semantic.cpp:5146-5208`)

---

## Medium Priority

- [ ] Destructor throw during exception unwinding loses original exception — `call_cleanup_destructor()` overwrites `vm->in_flight_exception` if the destructor throws, silently losing the original exception (`interpreter.cpp:61-97`)
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


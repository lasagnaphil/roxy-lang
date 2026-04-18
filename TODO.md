# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-04-19


---

## High Priority

- [x] F-string interpolation of a user-defined `Printable` struct requires an lvalue — fixed: `gen_string_interp_expr` no longer re-runs `gen_lvalue_addr` on the interpolated subexpression for the struct branch; it reuses the `gen_expr` result, which lowering already materializes as a struct pointer for all struct rvalues (per the convention in `gen_var_decl`).
- [ ] **Field-assigning a struct with an inner `when` clause into a variant field corrupts the inner discriminant** — `self.outer_field = inner_struct` (or `other.outer_field = inner_struct`) writes the inner struct's bytes into a variant slot but loses the discriminant for the nested tagged union. Repro: given `struct LoxValue { when kind: ValueKind { case VNumber: num_val: f64; ... } }` and `struct Expr { when kind: ExprKind { case ELiteral: literal_value: LoxValue; ... } }`, build `var v = make_number(42.0)` (v.kind = VNumber), then in a method/constructor assign `self.literal_value = v`. Afterwards `self.literal_value.kind` is neither VNumber nor any other valid variant. Downstream `when self.literal_value.kind` matches none of the cases, and `self.literal_value.num_val` triggers "variant field access with wrong discriminant" at runtime. In more complex cases (e.g. via `uniq Expr.binary(...)` named constructor), the corruption collides with the slab allocator and fires `slab->states[slot_idx] == SlotState::ALIVE` in `slab_allocator.cpp:286`. Struct literal construction (`Expr { ... literal_value = v }`) is unaffected; the bug is specific to *assignment* of a tagged-union-bearing struct into a variant field.
- [x] Module-local functions leak across module boundaries at call resolution time — fixed: IR builder now mangles non-pub function definitions and their same-module call sites with a `<module>::<name>` prefix, so distinct modules' non-pub functions no longer collide in the merged bytecode function table. `main` is exempt so the host can still invoke it via `vm_call("main", …)`; functions called from the host must be `pub`.

---

## Medium Priority

- [x] Variant (when-case) struct fields bypass cross-module visibility checks — fixed: parser now accepts `pub` on variant fields, and `analyze_get_expr` enforces the same `is_pub && !same_module` check as regular fields
- [x] Destructor throw during exception unwinding loses original exception — fixed: compile-time ban on `throw` in delete destructors + runtime safety net via `in_flight_exception` guard
- [x] Ternary expressions skip move-state merging — fixed: `analyze_ternary_expr` now saves/restores/merges move states across both branches, mirroring the if-statement pattern. Also fixed a pre-existing IR-gen bug where `gen_ternary_expr` returned `else_val` directly instead of a phi-merged block argument, causing the then-path to produce garbage at runtime
- [x] `alloc_large()` only zeros `size` bytes, not page-aligned `alloc_size` bytes — fixed: `alloc_large` now zeros the full page-aligned `alloc_size`, matching the slab path which zeros the full `slot_size`
- [x] Self-assignment of noncopyable values causes use-after-free — fixed: semantic analyzer rejects `x = x` when target is a noncopyable identifier resolving to the same symbol as the source
- [x] No definite-termination analysis for move-state merging — fixed: terminating branches (return/throw/break/continue) are now excluded from move-state merges in if/when/try; also fixed an IR-gen bug where `gen_assign_expr` didn't adopt the RHS temporary for noncopyable identifier targets, which surfaced once the analysis started accepting catch-reassignment patterns

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


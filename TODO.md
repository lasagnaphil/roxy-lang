# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-04-19


---

## High Priority

- [x] F-string interpolation of a user-defined `Printable` struct requires an lvalue — fixed: `gen_string_interp_expr` no longer re-runs `gen_lvalue_addr` on the interpolated subexpression for the struct branch; it reuses the `gen_expr` result, which lowering already materializes as a struct pointer for all struct rvalues (per the convention in `gen_var_decl`).
- [x] Two return paths each containing a `uniq` struct literal that differ only in nil-vs-value for one variant field double-free the shared local — fixed: the IR builder now mirrors the semantic-side definite-termination merge by saving `m_local_scopes` and `m_owned_locals.is_moved` before each branch in `gen_if_stmt` and restoring the appropriate snapshot at the merge block. Previously the then-branch's nullify-replace (which sets the local to nil after a struct-literal move) leaked into the merge block when the then-branch terminated, causing the after-if struct literal to embed nil and segfault on use.
- [x] Field-assigning a struct with an inner `when` clause into a variant field corrupts the inner discriminant — fixed: `gen_assign_expr` now uses `emit_get_field_addr` + `emit_struct_copy` for struct-typed field targets (matching the struct-literal path), instead of `emit_set_field` which was writing the source struct's pointer bits into the destination slots. The bug actually affected *all* struct-typed field assignment (regular and variant); nested-when discriminants made the corruption visible.
- [x] Assigning the result of a throwing call to a pre-declared struct local inside `try` crashes — fixed: `gen_try_stmt` now snapshots `m_local_scopes` before the try body and restores it before each catch (and the implicit finally-rethrow handler), so the catch sees the pre-try SSA bindings rather than the rebinding from the failed assignment. The bug also affected primitive return types (silently wrong value rather than crash) — register aliasing happened to mask it. `m_owned_locals.is_moved` is intentionally not rolled back, since that flag drives runtime cleanup bookkeeping; resetting it would re-enable the implicit-destroy preamble of `r = uniq T()` in catch and double-free already-consumed locals.
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


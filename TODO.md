# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-04-18


---

## High Priority

- [x] `bool` local inside a `when` arm reads back stale after an intervening `for` loop over `self.<list>.len()` — fixed. Root cause was the `fuse_compare_branch` peephole in `lowering.cpp`: any `CMP; JMP_IF_NOT` pair whose destination register matched the branch condition was fused into a single `JMP_IF_xx_I` two-word instruction, deleting the register write. That's correct when the compare's SSA result is only used by the immediately-following branch, but when the same SSA value feeds a later block's branch (here, the second `if (has_super)` after the for loop), the fused version leaves the register uninitialized — and with RPO block layout putting the loop body AFTER the second read, a loop-body temporary happily reassigned that register between the def and the second use. Fix: during lowering, every integer-compare emit records its bytecode PC in `m_unfusable_cmp_pcs` when its SSA result is cross-block (`!m_value_same_block`); `fuse_compare_branch` now skips those PCs. Resolver's `has_super` workaround (`examples/lox/resolver.roxy`) removed.

- [x] `inout List<uniq T>` parameter treated as a move across loop iterations — fixed. The TODO's root cause was correct for the *semantic* rejection (`check_call_args` called `consume_noncopyable` regardless of modifier), but lifting that ran the program straight into a family of parallel bugs on the IR side: `begin_function_body` tracked inout/out noncopyable params as owned locals (so the callee's scope-exit cleanup would double-free the caller's slot), `gen_call_expr`'s post-call move-nullify ran on inout/out identifier args (so the caller's local got nulled even though ownership stayed), and `collect_assigned_vars_expr` didn't treat inout/out identifier args as writes (so the loop header never phi'd the reloaded SSA value, and any post-loop read of the local tripped on a stale register). Fixes (all guarded on `arg.modifier != ParamModifier::None`): skip `consume_noncopyable` in `check_call_args`; skip owned-local tracking for `inout`/`out` params in `begin_function_body`; skip the post-call nullify-and-mark-moved loop in `gen_call_expr`; treat inout/out identifier args as writes in `collect_assigned_vars_expr` so while/for loops phi-merge the reloaded value.
- [x] Moving a noncopyable field out of a by-value struct parameter double-freed at runtime — fixed. Semantic analysis (`check_not_field_move`) correctly permits `self.x = param.field;` where `param` is a by-value struct and `param.field` is noncopyable, marking `param` as moved. The IR builder, however, was only copying the pointer into the target and never clearing the source field, so when `param`'s scope-exit destructor ran it destroyed `param.field` — already freed as part of the target's cleanup — and tripped the `slab->states[slot_idx] == SlotState::ALIVE` assert. Fix: `gen_assign_expr` now mirrors its identifier-source nullify for the ExprGet-source case. When the RHS is a field access on a local struct and both the target field and the source field are noncopyable, it emits a `SET_FIELD(null)` on the source field right after the copy. The destructor still runs on `param` but sees a null pointer for the moved field and safely skips via `Delete(null)`; unrelated sibling fields on `param` continue to destruct normally.

- [x] Named constructor assigning to a uniq (or List-of-uniq) field inside a `when`-variant crashed on the first call — fixed. Root cause was different from the TODO's hypothesis: the stale-field-bytes angle was already a non-issue for heap allocations (the slab zeroes slots on alloc), and the recent "null-init fields at constructor entry" fix now covers variant uniq fields too as defence in depth. The actual crash came from `gen_constructor_call` missing the move/consume fixup that `gen_call_expr` already does for noncopyable arguments: after calling `uniq Node.a(2, leaf)`, `leaf` in main still pointed at the slot the constructor had just stored into `self.child`, so when `parent`'s destructor freed `child` AND main's scope-exit Delete freed `leaf`, the second free tripped the `slab->states[slot_idx] == SlotState::ALIVE` assert. Fix: `gen_constructor_call` now calls `consume_temp_noncopyable` on each evaluated noncopyable argument (handles the inline-rvalue path, e.g. `Node.a(2, uniq Node.b(…))`) and emits the same post-call identifier-nullify + `is_moved=true` that regular function calls already did (handles the named-local path, e.g. `Node.a(2, leaf)`). Also extended the earlier "null-init own uniq/noncopyable fields at constructor entry" to include variant fields, so struct layouts whose only uniq data lives inside a when-clause union are covered on non-zeroed allocations too.
- [x] User constructor assigning a uniq/`List<uniq T>`/etc. field double-freed across sequential constructions (both variants: the constructor itself calling `List<uniq T>()` and later reassigning, *and* the constructor taking a `List<uniq T>` parameter and storing it in the field) — fixed. Root cause: `self.field = value` inside a user-defined constructor flows through `gen_assign_expr`, which for uniq/noncopyable fields emits a destroy-of-the-old-value preamble. At entry to the constructor, `self`'s fields hold whatever bytes the caller's return-slot still contained — on a fresh frame that's usually zero (`Delete(null)` is a safe no-op, hence "silently OK on the first call"), but on the second call the same local_stack offset holds the first call's (now-freed) pointer, so `Delete` runs on a dead slab slot. Fix: `build_constructor` now null-initialises every own uniq/noncopyable field at entry (after the parent-constructor call, before user code), mirroring what `build_synthesized_default_constructor` already did via direct `emit_set_field`. The first `self.field = …` now destroys a null, and the subsequent bytes are written by the user code as intended. Struct-literal construction was unaffected because it never went through the destroy-old preamble.
- [x] F-string interpolation of a user-defined `Printable` struct requires an lvalue — fixed: `gen_string_interp_expr` no longer re-runs `gen_lvalue_addr` on the interpolated subexpression for the struct branch; it reuses the `gen_expr` result, which lowering already materializes as a struct pointer for all struct rvalues (per the convention in `gen_var_decl`).
- [x] Two return paths each containing a `uniq` struct literal that differ only in nil-vs-value for one variant field double-free the shared local — fixed: the IR builder now mirrors the semantic-side definite-termination merge by saving `m_local_scopes` and `m_owned_locals.is_moved` before each branch in `gen_if_stmt` and restoring the appropriate snapshot at the merge block. Previously the then-branch's nullify-replace (which sets the local to nil after a struct-literal move) leaked into the merge block when the then-branch terminated, causing the after-if struct literal to embed nil and segfault on use.
- [x] Field-assigning a struct with an inner `when` clause into a variant field corrupts the inner discriminant — fixed: `gen_assign_expr` now uses `emit_get_field_addr` + `emit_struct_copy` for struct-typed field targets (matching the struct-literal path), instead of `emit_set_field` which was writing the source struct's pointer bits into the destination slots. The bug actually affected *all* struct-typed field assignment (regular and variant); nested-when discriminants made the corruption visible.
- [x] Assigning the result of a throwing call to a pre-declared struct local inside `try` crashes — fixed: `gen_try_stmt` now snapshots `m_local_scopes` before the try body and restores it before each catch (and the implicit finally-rethrow handler), so the catch sees the pre-try SSA bindings rather than the rebinding from the failed assignment. The bug also affected primitive return types (silently wrong value rather than crash) — register aliasing happened to mask it. `m_owned_locals.is_moved` is intentionally not rolled back, since that flag drives runtime cleanup bookkeeping; resetting it would re-enable the implicit-destroy preamble of `r = uniq T()` in catch and double-free already-consumed locals.
- [x] Try/catch around a `for`/`while` loop didn't catch exceptions thrown from inside — fixed. Root cause: `BCExceptionHandler` carried a single `[try_start_pc, try_end_pc)` window, computed in lowering as "offset of try_entry" through "offset of the block whose id = try_exit + 1". That assumed try-body blocks are laid out contiguously in the final bytecode, which is true in creation order but breaks after `reorder_blocks_rpo` — a `while` inside a try has its body block laid out after the loop's fall-through `try.after` because RPO post-orders the back-edge target last, so the body block ends up outside the single window. The throw in `deep()` happened at a pc past `try_end_pc` and was treated as unhandled. Fix: `IRExceptionHandler` now carries the full set of block ids that belong to the try body (`try_body_blocks`), populated in creation order by `gen_try_stmt`, remapped by `reorder_blocks_rpo` (and the coroutine-lowering remap); lowering sorts the remapped layout positions and emits one `BCExceptionHandler` per contiguous run of ids, all sharing the same `handler_pc`/`type_id`. The runtime's linear scan over handlers already stops at the first match, so non-contiguous try ranges are cheap.
- [x] Reading a negative `i32` field out of a struct or a `List<i32>` element didn't sign-extend, so `v == -1` was false even though `print(v)` showed `-1` — fixed. All other 32-bit integer loads (LOAD_INT, arithmetic results) already sign-extend into their 64-bit registers, which is the invariant `reg_as_i64` relies on; `GET_FIELD` (for `slot_count == 1`) and `INDEX_GET_LIST` (for 1-slot inline elements) were zero-extending, leaving a negative i32 as `0x00000000FFFFFFFF = +4294967295`. Same issue in `INDEX_GET_MAP` / `native_map_get`'s inline-value path for the new variable-sized map values. All four paths now sign-extend via `static_cast<i64>(static_cast<i32>(…))`. Unsigned 8/16 bit fields are unaffected (their max value < 2³¹); u32 fields with values above 2³¹ now behave like the corresponding signed i32 — consistent with how u32 locals are already held across arithmetic ops (everything goes through `reg_as_i64`).
- [x] Map<K, V> with a struct-typed V corrupts stored values across method boundaries — fixed. Root cause (different from the "register aliasing" hypothesis): `MapHeader` stored values as a fixed `u64* values`, so struct values larger than 8 bytes were handed to `native_map_insert` as a *stack pointer* to the value's bytes on the caller's local_stack. The moment that frame popped (i.e. the method returned), the pointer dangled; the next stack-materializing call — another struct-returning method, another struct-valued insert, a `new Env(0)` small-struct return — reused the same local_stack offset and silently overwrote the bytes the dangling pointer referenced. This is what "insert(a) then insert(b) reads back b for both" looked like (both values shared the same local_stack slot), and what "insert then push stomps the value" looked like (the push's constructor return materialized over the previous value's bytes).<br>Fix: Map now stores variable-sized values inline, mirroring List's element layout. `MapHeader` gains `value_slot_count` + `value_is_inline`; bucket arrays are sized `value_slot_count * 4 * capacity`; insert/get/remove/grow/copy/clear operate in bytes; `native_map_get` / `INDEX_GET_MAP` return a pointer into the map's backing storage for struct values; `INDEX_SET_MAP` / `native_map_insert` read the source as bytes for inline values or dereference a struct pointer for struct values; the IR builder threads `value_slot_count` / `value_is_inline` into `native_map_alloc` at `Map<K, V>()` construction. Robin-Hood swap uses ping-pong scratch buffers so multi-eviction chains in `map_insert_internal` don't clobber the value being placed.
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


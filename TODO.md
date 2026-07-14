# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-07-14

---

## High Priority

(none currently)

---

## Medium Priority

(none currently)

---

## Low Priority

- [ ] **Register overflow on huge single functions**: a function with more than
  ~255 simultaneously live values fails bytecode lowering with "Register
  overflow: function uses too many values (max 255)" despite the
  furthest-first spilling — observed 2026-07-06 on generated 8k-statement
  bodies where every statement binds a string/uniq local that stays live to
  scope end (the adversarial linear-scan workloads). Diagnose whether spilling
  is bypassed for cleanup-record-pinned values or simply capped;
  pathological-input-only today (untested whether the C backend, which does
  no register allocation, accepts the same inputs).

---

## Planned Features

- [ ] **Coroutine methods on generic structs / traits**: non-generic instance coroutine methods (`fun S.count(): Coro<i32>`) landed 2026-07-12 (`self` captured as a `ref` param, classified by `MethodDecl::is_coroutine`; see `docs/internals/coroutines.md` → "Coroutine Methods"). The generic-struct (`fun Box<T>.gen()`) and trait cases are rejected with a clear "not yet supported on generic structs or in traits" error. Generic support needs the classification threaded through the generic monomorphization path (`register_generic_struct_method` + instantiation); traits need it through `resolve_trait_impl_member`/`validate_and_register_impl_method` (which currently don't handle a `Coro<T>` return).
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration). Must keep the fresh-AST-per-analysis shape required by the single-shot analysis rule (see the annotation-contract block in `ast.hpp`); if it ever forces re-analyzable ASTs, revisit the decision to keep lambda-capture analysis inline in the analyzer.
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)

---

## Code Quality Improvements

- [ ] Standardize error message formatting across the compiler: consistent quoting, capitalization, and terminology in diagnostics. (The compile-time `{}`-placeholder/argument-count check that shared this item landed 2026-07-06 — the build was bumped to C++20 and `rx::fmt_string`'s `consteval` constructor now rejects placeholder/arg count mismatches at build time for literal format strings passed to `format_to`/`format`/`error_fmt`/`intern_format`/…; `rx::runtime()` is the escape hatch for dynamic format strings. See `core/format.hpp`.)

---

## Documentation Needed

- [ ] Document thread-safety limitations (single VM per thread assumed)

---

## Semantic Analyzer Refactoring

Residuals from the 2026-07-05 deep review of `semantic.cpp` and collaborators. The
rest of that backlog is done and removed from this file: the god-class split
(LifetimeChecker / TraitSystem / GenericCallResolver behind a shared SemaContext),
per-function context bundling, the naming-inversion fix, the semantic→IR annotation
contract (`ast.hpp`), the single-shot analysis rule, the never-null
`resolve_type_expr` contract, all eight duplication cleanups, all eight
correctness-adjacent debts, and three of the four Performance items (`pop_scope`
shadow-restore-on-define, O(1) `lookup_local`, `append_span` geometric growth) —
per-item records in git history.

- [ ] **Define a single LSP-mode null-tolerance policy**: some walker paths null-check every child, others assume `decl`/info chains are present. The concrete `analyze_constructor_call` crash is fixed, but the analyzer still lacks a stated per-pass rule for what may be null in LSP-recovered ASTs. A policy matching current reality: after the declaration passes every registered Info has non-null types but may have a null `decl`; body walkers must tolerate null AST children but may assume resolved `Type*` non-null (the `resolve_type_expr` contract).
- [ ] Move-state snapshots copy the whole map at every branch point (if/while/for/when/try/ternary) — fine at current scale; revisit with an undo log only if profiling warrants. **Measured 2026-07-05**: on a deliberately hostile workload (400 fns × 50 uniq locals × 50 if/else each), semantic analysis was ~20% of compile time and the `Symbol*→MoveState` map churn a minor slice of that — IR build and bytecode lowering dominate (`IRBuilder`'s per-scope `Vector<robin_map>` copies and `compute_liveness` were the top profile entries; a separate, lowering-side question if it ever matters).

---

## IRBuilder Refactoring

From a 2026-07-05 deep review of `ir_builder.{hpp,cpp}` (~7,600 lines). The
structural items are done and removed from this file — the four-way TU split,
the `ir_fold` extraction, and (2026-07-06) the `OwnershipTracker` collaborator
(`ownership_tracker.{hpp,cpp}`: owned-local state + keyed name/value lookups,
replacing the hot-path linear scans; sound because local shadowing is banned)
together with the `collect_assigned_vars` seen-set dedupe — per-item records
and measurements in git history. Remaining:

- [ ] `find_method_fn_index` still scans all module functions by name —
  deliberately kept: cold path (struct-keyed map constructors only, ≤2 scans
  each), and an incremental name→index map would need maintenance at every
  build-phase push_back for no measurable win.

---

## Bytecode VM Opcode Improvements

From a 2026-04-26 review comparing Roxy's opcode set against Lua 5.4, LuaJIT,
CPython 3.13, Wren, JVM, and V8 Ignition. The base design (register-based, 32-bit
fixed-width ABC/ABI/AOFF, computed-goto dispatch, type-specialized arithmetic,
fused i64 compare+branch) is Lua-class and sound. The high-ROI deltas (RK operand
encoding, 32-bit `CALL` func_idx, fused f64 compare+branch) and two medium-ROI ones
(`AND`/`OR` removal, specialized small-struct copy) have landed — records in git
history. Remaining:

- [ ] **Inline-cache slot in `CALL_METHOD` for trait/vtable dispatch.** Not needed today (no virtual dispatch yet), but cheap to design in now and painful to retrofit. Reserve 1–2 words per call site for resolved function pointer + monomorphic guard. Partially pre-satisfied: the 2026-04-28 `CALL` widening already reserved the upper bits of its 32-bit func_idx word for inline-cache slots / tail-call flags. Reference: V8 Ignition feedback vectors, Smalltalk PIC.
- [ ] **Wider immediate for `LOAD_INT`.** Currently signed 16-bit; constants outside ±32K hit the constant pool. Lua 5.4 added `LOADI` with 24-bit signed sBx. Worth it only if profiler shows meaningful `LOAD_CONST` traffic for small-but-out-of-range integers.

---

## Testing Gaps

- [ ] Add fuzzing for parser/lexer

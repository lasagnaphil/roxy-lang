# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-07-05

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

- [ ] **Unsigned & small-int arithmetic** (u8/u16/u32/u64/i8/i16 beyond `==`/`!=`): needs unsigned IR ops (compare/div/mod/shr), width-wrapping semantics for narrow arithmetic (post-op narrowing or dedicated opcodes), VM opcode support, and a C-backend signedness audit (it emits from the same signed-i64 IR). The semantic layer rejects these with "operator not supported" instead of silently mistyping (since 2026-07-05).
- [ ] **First-class coroutine values**: passing or returning a `Coro<T>` fails assignability — an annotated `Coro<T>` resolves to the interned generic type while a coroutine value carries its per-function type (`coroutine_type_for_func`, which encodes the mangled `resume`/`done` names). Needs unified typing plus dynamic resume/done dispatch (the closure design — `__call_idx` + `CALL_INDIRECT` — is the obvious template). Inference already binds `T` from a coro argument.
- [ ] **Coroutine methods** (`fun S.count(): Coro<i32>`): rejected with an explicit "coroutine methods are not yet supported" error (since 2026-07-05); needs the per-function coro type for methods plus a `self`-carrying state machine in coroutine lowering.
- [ ] **Casting unsuffixed integer literals to small int types** fails: `u8(200)` → "cannot cast 'i32' to 'u8'" because the literal has IntLiteral kind, which `TypeChecker::can_cast` doesn't treat as numeric. Casting a *variable* works. Coerce the literal (or accept IntLiteral) in `analyze_primitive_cast`.
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements. Also the blocker for two diagnostics: the all-paths-return check (see the note in `analyze_fun_body` — `branch_terminates()` deliberately reads a no-else `when` as non-terminating), and sharper move-state merges (an exhaustive no-else `when` currently keeps the pre-when fall-through as a live path). The IR builder's fall-through block for a no-else `when` must agree with whatever semantics is chosen.
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration). Must keep the fresh-AST-per-analysis shape required by the single-shot analysis rule (see the annotation-contract block in `ast.hpp`); if it ever forces re-analyzable ASTs, revisit the decision to keep lambda-capture analysis inline in the analyzer.
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)

---

## Code Quality Improvements

- [ ] Standardize error message formatting across the compiler; consider a compile-time `{}`-placeholder check for `error_fmt`/`format_to` (the printf-residue bugs fixed 2026-07-05 would have been caught at build time). A cheap variant: a debug-build assert comparing `{}` count against the argument count in `ErrorReporter::error_fmt`.
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis (pairs naturally with the LSP null-tolerance policy item below — same audit)
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

From a 2026-07-05 deep review of `ir_builder.{hpp,cpp}` (~7,600 lines). The code is
semantically sound and well-commented; nearly all findings are structural
duplication — the same machinery copy-pasted across statement kinds, and the same
emission idioms repeated dozens of times. Correctness-adjacent items first.

- [ ] **Split the TU / extract collaborators** (the semantic.cpp precedent):
  (a) zero-risk `.cpp` split into decls/stmt/expr/lifetime files; (b) easy
  extraction of Phase-1 folding (`try_fold_*` / `try_simplify_*`, ~350
  nearly-stateless lines) into `ir_fold.{hpp,cpp}`; (c) longer-term
  `OwnershipTracker` collaborator for `m_owned_locals` + consume/move/track/
  cleanup-record machinery, mirroring `LifetimeChecker`.
- [ ] Linear-scan lookups: `find_method_fn_index` scans all module functions by
  name (cold — struct-keyed map ctors only); `find_owned_local` scans by name on
  hot paths; `collect_assigned_vars` dedupe is O(n²). Small-N today; swap to keyed
  maps if they ever show in a profile.
- [ ] `StringView("insert", 6)`-style manual lengths throughout — add a constexpr
  literal helper (`"…"_sv` or sizeof-based) to remove the miscount hazard.
  (Codebase-wide, but densest here.)

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
- [ ] **Verify i32 `DIV_I`/`MOD_I` edge cases.** All integer ops route through i64 (`interpreter.cpp:688-705`). Add/sub/mul are bit-identical between i32 and i64 wrapping, but `INT32_MIN / -1` differs (i64 path silently produces a different result; i32 hardware would trap). Confirm SSA lowering inserts a `TRUNC_S` or that this case is unreachable — and check what the C backend emits for the same IR (native i32 division here is UB in C). A two-test probe on both backends settles it.

---

## Testing Gaps

- [ ] Test deeply nested struct field access (>5 levels; currently only 3 levels tested)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs (diamond dependencies, >3 levels)

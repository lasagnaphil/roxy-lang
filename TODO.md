# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-08-09

---

## High Priority

- [ ] **`Map.get()` of a noncopyable *container* value double-frees**: binding
  `m.get(k)` to a local moves the value out from under the map, and both the new
  owner and the map's own cleanup free it — `delete_value` aborts with
  "double-delete: heap object already freed". Repro (2026-08-09, on an unmodified
  tree):
  ```roxy
  var m: Map<i32, List<i32>> = Map<i32, List<i32>>();
  m.insert(0, inner);
  var stolen: List<i32> = m.get(0);   // double-delete
  ```
  It falls through *both* guards, in the seam between them. `Map.get` is typed
  `borrowed V`, but `borrowed` is the **identity** for `List`/`Map`/`Coro`/value
  structs (only `uniq T` and `fun` demote to a borrow), so the result stays an
  owning `List<i32>`; and `LifetimeChecker::consume_noncopyable`'s move-out guard
  fires only for `AstKind::ExprIndex`, while `.get(k)` is a call. Each mechanism
  covers what the other misses *except here*. The `m[0]` form of the same program
  is correctly rejected, and `Map<i32, uniq Point>.get(0)` is correctly rejected
  (that one `borrowed` does demote). Fix: generalize the guard from `ExprIndex` to
  native-method call results. That is safe against over-rejection — the guard is
  only reached when the *declared target* type is noncopyable (`semantic.cpp:1539`),
  so `var b: ref P = m.get(0)` is unaffected.
  Note the guard is itself **untested**: disabling it entirely leaves the whole
  suite green, yet `var stolen: List<i32> = outer[0]` on a `List<List<i32>>` then
  double-frees. Both holes want regression tests. The comment at
  `lifetime_checker.cpp:306` calling the guard "the interim rule until
  `borrowed`-typed returns make the result a `ref`" is stale and should go — for
  the identity kinds the guard is the *only* cover, not an interim one.

*(empty otherwise — the teardown leak check added 2026-08-02 (`roxy --check-leaks`; the
E2E harness asserts it on every program it runs) surfaced a family of
exception-unwind leaks that is now fully fixed: `examples/lox/test.roxy` runs
at **0 live objects** at teardown, from 293 on 2026-08-02. The last one — a
cleanup record is a single linear PC interval, but RPO lays a throw-terminated
branch out *after* the scope's normal-exit block, so the interval truncated at
the Nullify missed the throw and the owned value leaked — was fixed by
computing per-record block coverage, pinning the register across it, and
emitting extension records for covered runs outside the main interval; see
`docs/internals/lifetimes.md` → "What the flip exposed in the unwind path" and
the tail of `tests/e2e/test_exceptions.cpp`.)*

*The point worth keeping from how these were found: twenty-two further bugs were
fixed here — five in coroutines, three in destructor chaining, two `ref`-counting
holes, one in operator parsing (all 2026-08-02), and on 2026-08-08 a caught
exception leaked by a coroutine destroyed while suspended inside its `catch`, an
undropped by-value container parameter of a method, divergent conditional moves
(which leaked in an `if` and double-freed in an `if/else if` chain),
name-conflated coroutine state fields (a segfault), `when` arms that skipped
their scope cleanup after the first one, a variant field storing a string it
did not own, a string temporary adopted by its binding whose duplicate cleanup
record released it twice while unwinding, a cleanup record whose end block came
from the last block *created* rather than the block emission was last in, and a
local reassigned in a branch whose record still named its pre-merge register, and
a cleanup record whose end block the optimizer dropped as unreachable and whose
range therefore collapsed onto the entry block, and a throwing branch laid out
past the scope's normal exit that no single-interval record could cover, and a
`weak`/`ref` local reassigned from a `uniq` owner that was wrongly treated as a
move of the owner (later reads dereferenced null, and the skipped scope-exit
Delete leaked it; `tests/e2e/test_lifetime_regressions.cpp` F10) —
and every one of them was invisible to a fully green suite. Nine
came from compiling `CLAUDE.md`'s example program, which had never been run; it
now compiles and runs verbatim. Per-bug records are in this file's git history.*

---

## Medium Priority

- [ ] **A discarded borrow is held to the end of its enclosing scope, not its
  statement**: `print(f"{box.borrow_item().v}"); delete box;` trips the
  free-trap, because the temporary borrow that the first statement created is
  only released when the scope closes. The count is no longer *leaked* — the
  discarded `ref`-call borrow that leaked its handed-off count was fixed
  2026-08-02, so the count is released, and per-iteration inside a loop body — so
  this is a lifetime-narrowing question rather than a leak, and it is the same
  rule every temporary follows: wrapping the use in an
  inner block, or binding the borrow to a `ref` local, both work today. Pinned by
  "deleting an owner in the same scope as a discarded borrow still traps" in
  `tests/e2e/test_lifetimes.cpp`. Narrowing a temporary to its statement needs a
  statement-scoped temp mechanism the IR builder doesn't have; note that the
  obvious version (release at the end of the enclosing statement) is **wrong for
  a borrow created in a loop *condition***, which re-acquires every iteration but
  would be released once. Owned and string temporaries have the same
  loop-condition shape; theirs is invisible because a delayed free only costs
  memory, whereas a delayed borrow blocks a `delete`.
- [ ] **Rebinding a `ref` binding to a fresh owner compiles and then traps at
  runtime**: `fun f(r: ref List<i32>) { r = List<i32>(); }` (and the identical
  `fun f(p: ref P) { p = uniq P(); }`) type-checks — the source converts to the
  target `ref` type — but the rebind neither releases the old borrow's count nor
  takes one on the new object, so the program dies with "ref_dec: reference
  count already zero". Pre-existing for `uniq`, verified 2026-08-02 on an
  unmodified tree; the container-borrow work made the same shape reachable for
  `List`/`Map`. The model has no stated rule for rebinding a live borrow: either
  reject assignment to a `ref`-typed binding whose source is an owning value, or
  make the rebind emit `RefDec(old)` + `RefInc(new)`. Rejecting is the smaller
  change and matches "a `ref` names one object for its lifetime".
  Binding a `ref` *local* to an unowned temporary (`var r: ref P = uniq P();`,
  `var r: ref List<i32> = List<i32>();`) fails the same way and is the same
  family — nobody owns the temporary, so the local's scope-exit `RefDec` has no
  matching increment. Also verified pre-existing for `uniq`. Passing a temporary
  as a borrow *argument* (`take(List<i32>())`) is fine: the caller frame keeps
  and drops it.

---

## Low Priority

- [ ] **Call depth is capped at 1024 frames with no way to raise it**:
  `VMConfig::max_call_depth` defaults to 1024 (`vm/vm.hpp`) and `roxy.cpp` never
  overrides it, so a recursive program deeper than ~1020 frames dies with
  "Call stack overflow" and the only recourse is rewriting it with an explicit
  stack. An embedder can set the config; a CLI user can't. A `--max-call-depth`
  flag (and the matching `--register-file-size`, currently 65536) would cost
  little. Verified 2026-07-17: `depth(1000)` returns, `depth(10000)` overflows.
- [ ] **String stdlib gaps**: the primitives are `str_len`, `str_char_at`,
  `str_substr`, `str_concat`, `str_eq`/`str_ne`, `str_from_code`, `str_to_f64` —
  no `split`, no integer parse (only `str_to_f64`), so any text handling starts
  by hand-rolling both. `str_concat` in a loop is quadratic (20k single-char
  appends measured at 0.34s on the `-O0` build, 2026-07-17); a builder, or a
  `join`, would remove the usual reason to write that loop.
- [ ] **LSP parser super-linear memory on adversarial input**: `fuzz_lsp_parser`
  found an OOM — a mutated ~8 KB Lox source (near the `-max_len=8192` cap) drives
  the error-recovering parser to allocate ~2.9 GB (≈370,000× blow-up), so the
  allocation is super-linear (likely O(n²) bump-allocated CST nodes from
  overlapping trial-parses / error recovery; *not* generic `<…>` trial-parsing —
  the reproducer has a single `<`). Terminates (no hang) but exhausts memory.
  Needs allocation profiling to pin the quadratic site; a cap on total CST node
  count / recursion depth (bail to a truncated tree) is the likely mitigation.
  Found 2026-07-14; do **not** add the reproducer to `tests/fuzz/corpus/` — the
  `Fuzz Regression` doctest replays it with no memory cap. Reproduce with
  `./build-fuzz/fuzz_lsp_parser -rss_limit_mb=2048 <repro>`.

---

## Planned Features

- [ ] **Coroutine methods on generic structs / traits**: non-generic instance coroutine methods (`fun S.count(): Coro<i32>`) landed 2026-07-12 (`self` captured as a `ref` param, classified by `MethodDecl::is_coroutine`; see `docs/internals/coroutines.md` → "Coroutine Methods"). The generic-struct (`fun Box<T>.gen()`) and trait cases are rejected with a clear "not yet supported on generic structs or in traits" error. Generic support needs the classification threaded through the generic monomorphization path (`register_generic_struct_method` + instantiation); traits need it through `resolve_trait_impl_member`/`validate_and_register_impl_method` (which currently don't handle a `Coro<T>` return).
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration). Must keep the fresh-AST-per-analysis shape required by the single-shot analysis rule (see the annotation-contract block in `ast.hpp`); if it ever forces re-analyzable ASTs, revisit the decision to keep lambda-capture analysis inline in the analyzer.
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)

---

## Code Quality Improvements

(none currently)

---

## Documentation Needed

- [ ] **`lifetimes.md`'s status table contradicts the sections below it**:
  "What is actually implemented" (§ *Lifecycle implementation and status*) still
  reports all three rows as they stood *before* the Drop/Copy separation landed
  on 2026-08-02 — Drop "complete except `StrRelease` on a struct field, gated
  off", Retain "derived but unwired", Move-only "mis-derived". All three are
  superseded ~40 lines later by *Separating Drop from Copy* ✅, and the code
  agrees with the later text: `member_needs_drop` (`types.hpp`) has no
  `StrRelease` carve-out, `member_needs_retain` consumes `compute_retain_plan`,
  and `noncopyable()` reads the structural `is_move_only` flag. That table is the
  first thing a reader touching lifecycle code will find, so it is the worst
  place in the tree for stale status. `CLAUDE.md`'s one-line `lifetimes.md`
  blurb repeats the same three stale claims and needs the same fix.
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

- The IR optimizer is now covered end-to-end (**landed 2026-08-02**). Every
  pipeline builder in `tests/e2e/test_helpers.cpp` routes through the single
  `build_ir()` helper, which calls `optimize_module()` between
  `coroutine_lower` and `IRValidator`, matching `Compiler::link_modules()`, so
  the whole parametric suite exercises optimized IR on both backends. Before
  this, the optimizer's only coverage was `tests/unit/test_ir_optimize.cpp`'s
  hand-built functions — which is how a crash on *every* coroutine program sat
  in a fully green suite. **Keep the harness and `Compiler::link_modules()` in
  the same order**; a divergence there is invisible until something reproduces
  on the CLI but not in tests.

- Fuzzing for the lexer/parser/LSP parser **landed** — coverage-guided libFuzzer
  targets in `tests/fuzz/` (`fuzz_lexer`/`fuzz_parser`/`fuzz_lsp_parser`, built
  via `-DENABLE_FUZZERS=ON`) plus an always-on `Fuzz Regression` doctest suite
  that replays the seed corpus + `examples/` through all three harnesses. See
  `tests/fuzz/README.md`. The initial campaign found and fixed three real bugs:
  two LSP error-recovering-parser infinite loops (`when self.<member>`
  discriminant; stray leading tokens like `}`/`"`/`,` with no forward-progress
  guard) and a lexer signed-overflow UB on out-of-range integer literals. One
  finding remains open ↓.
- [ ] **Structure-aware fuzzing** (design plan in `docs/internals/fuzzer.md` →
  Roadmap): byte-level mutation plateaus at the parser — reaching sema / IR /
  lowering / VM / C backend needs valid-by-construction programs. Staged: (1) a
  grammar generator via libprotobuf-mutator for the parser + sema reject paths;
  (2) scoping + type-directed generation to reach the IR builder/VM, with a
  **VM-vs-C-backend differential** oracle (`compile_and_run` vs
  `compile_and_run_cpp`) that catches miscompiles unit tests miss; (3) `uniq`
  move-state modeling to reach RAII/drop/codegen paths.

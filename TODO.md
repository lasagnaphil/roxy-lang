# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-08-08

---

## High Priority

*The leak entries here were found by the teardown leak check added 2026-08-02
(`roxy --check-leaks`; the E2E harness asserts it on every program it runs).
They were invisible before: the free-trap only fires on an explicit `delete`, so
nothing was looking.*

- [ ] **Scope cleanup on the exception-unwind path is wrong in both directions**,
  which is what remains of the Lox leak. Narrowed 2026-08-08 to two minimal
  repros; both are clean the moment the `throw` is removed, so this is the
  unwind path specifically, not ordinary scope exit.
  *(in progress — see `HANDOFF.md` for state, traps, and the tracing method)*

  *Over-release — a moved-out struct temporary is destroyed anyway.* The
  temporary that a struct literal builds is copied into the heap exception
  object and marked moved (`nullify`), but its cleanup record still fires during
  unwind, releasing the string field the heap object now owns. The count reaches
  zero while the handler is still using it, so the value is **gone by the time
  the catch reads it**:
  ```roxy
  struct E { v: string; }
  fun E.message(): string for Exception { return "e"; }
  fun f(a: string) { var s: string = a + "!"; throw E { v = s }; }
  fun main(): i32 { var x: string = "dyn";
      try { f(x); } catch (e: E) { print(e.v); }   // prints an EMPTY line
      return 0; }
  ```

  *Under-release — a value-struct local is not destroyed at all.* Same shape with
  the local being a value struct that owns a counted member: nothing releases it
  while unwinding, so it leaks. (Replace `s` above with
  `var val: V = V.of_str(a + "!")` where `V` is a tagged union with a `string`
  variant, and throw `E { v = val }`.)

  Both point at `record_scope_cleanup_records` / the Nullify-narrowing of
  cleanup records: it records every owned local in scope without consulting
  `is_moved`, and narrowing evidently does not cover a moved struct temporary,
  while a value-struct local seems not to produce a firing record at all.
  Together they are most of the Lox residue: `examples/lox/test.roxy` leaks 33
  objects (31 strings + 2 lists), down from 293 on 2026-08-02. Lox reaches this
  path on every function return, since `return` is implemented by throwing a
  `ReturnException`. Remaining alloc sites, for cross-checking a fix:
  `call_fun` ×12, `eval_binary` ×4, `call_native` ×3, `eval_get` ×2, plus ~9
  scanner strings reached only through the `expect_*_error` tests. The 2 lists
  are specific to `test.roxy` and no longer scale with interpreter calls.

  Method that found this, worth reusing: tag each allocation with the VM call
  stack, then trace every `ref_count` transition of one leaked object and pair
  each retain with its release — the unmatched retain names the owner. That
  turned a day of fruitless shape-guessing (~10 hand-written repros of the
  "obvious" shape, all clean) into a diagnosis in minutes.

*The point worth keeping from how these were found: seventeen further bugs were
fixed here — five in coroutines, three in destructor chaining, two `ref`-counting
holes, one in operator parsing (all 2026-08-02), and on 2026-08-08 a caught
exception leaked by a coroutine destroyed while suspended inside its `catch`, an
undropped by-value container parameter of a method, divergent conditional moves
(which leaked in an `if` and double-freed in an `if/else if` chain),
name-conflated coroutine state fields (a segfault), `when` arms that skipped
their scope cleanup after the first one, and a variant field storing a string it
did not own — and every one of them was invisible to a fully green suite. Nine
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

# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-08-02

---

## High Priority

*Found by the teardown leak check added 2026-08-02 (`roxy --check-leaks`; the
E2E harness asserts it on every program it runs). Both were invisible before:
the free-trap only fires on an explicit `delete`, so nothing was looking.*

- [ ] **A coroutine suspended inside a `catch` leaks the caught exception when
  destroyed without being drained**: `fun gen(): Coro<i32> { try { throw MyErr {}; } catch (e: MyErr) { yield 42; } }`
  followed by one `resume()` and no drain leaves the exception object alive.
  Yielding from inside the catch promotes `e` into the coroutine's state struct;
  the resume path deletes it (`get_field self.e` → `delete`), but the generated
  `__coro_*$$delete` never does, so an undrained coroutine frees the state struct
  and drops the exception on the floor. Cause: `generate_coro_destructor` selects
  fields with `member_needs_drop(field.type)`, and an exception is an *ordinary
  copyable struct* — it is owned by the catch scope, not by its type
  (lifetimes.md "Caught exceptions"). This is the same "decided from the wrong
  source of truth" shape as the return-path bugs fixed the same day.
  Fixing it needs both halves: include catch-param fields in `$$delete`, **and**
  null the field on the resume path after the existing delete (the pattern the
  ref-local `RefDec` → `SetField(null)` rewrite in `coroutine_lower` already
  uses), or a drained coroutine double-frees. Narrowed to needing *both* a yield
  inside the catch and an undrained coroutine — yield outside the catch, a fully
  drained coroutine, and a throw/catch in a callee are all clean. Pinned by
  `ExpectedLeak` in the two `E2E Coroutines` cases; remove those when fixed.
- [ ] **A `string` field in a struct is retained on store but never released**
  — blocked on separating Drop from Copy. `struct Box { s: string; }` leaks the
  string; this is most of the Lox string leak (`Scanner.source`,
  `Token.lexeme`, …). Minimal repro:
  `struct Box { s: string; } fun main(): i32 { var a: string = "x"; var d: string = a + "y"; var b: Box = Box { s = d }; print(b.s); return 0; }`

  The *gate* is now single-sourced (`member_needs_drop` derives from
  `compute_drop_plan`, 2026-08-02), and both backends have always lowered
  `DropKind::StrRelease` correctly — so the release side is one line away.
  `StrRelease` is explicitly excluded there, because turning it on alone is
  wrong in two ways, both measured:

  1. **It makes string-bearing structs move-only.** `noncopyable()` on a struct
     means literally "has a default destructor", so the synthetic destructor a
     string field would earn also flips the struct to move-only. Enabling it
     broke four `Structured Gen` cases with "self-assignment of noncopyable
     variable" / "use of possibly moved value". `struct Point { name: string; }`
     should stay copyable.
  2. **Fixing (1) without clone glue is worse than the leak.** A copyable struct
     is copied bitwise (`IROp::StructCopy`), so two structs would share one
     string and both release it — use-after-free.

  The two-part fix:

  - **Decouple move-only from drop-glue.** Give `StructTypeInfo` an
    `is_move_only` flag computed by the existing synthetic-destructor fixpoint:
    true iff a field is itself move-only (`uniq` / `List` / `Map` / `Coro` /
    closure / move-only struct) or the struct has a *user-written* default
    destructor. `noncopyable()`'s struct arm reads that instead of "has any
    default destructor". Should be a no-op for every type today — the structs
    that currently earn a synthetic destructor are exactly the move-only ones.
  - **Add clone glue at the duplication sites.** `compute_retain_plan` (landed
    2026-08-02) is the derivation; what remains is emitting it. The site list is
    longer than `emit_struct_copy` — by-value struct arguments and small struct
    returns duplicate in *bytecode lowering* with no `StructCopy` in the IR, and
    container-element and struct-field reads duplicate too. See lifetimes.md →
    "Duplication sites the glue must cover". Missing one turns the leak into a
    use-after-free, so route duplication through an op that carries the
    obligation rather than patching sites.

  **Ordering:** the move-only change and the glue+gate change must land
  **together**. Each alone is unbalanced — retains without releases, a
  destructor that forces move-only, or copyable `ref` structs with unbalanced
  counts. lifetimes.md → "The ordering constraint" has the table.

  This is the Clone half of the value-lifecycle model (lifetimes.md "Value
  lifecycle"), which is described there but only implemented for Drop. The
  teardown leak check plus the VM's double-delete and release-at-zero asserts
  make it verifiable; do it as its own change, not bundled.

- [ ] **The Lox interpreter leaks one List per interpreter call**:
  `fun f(n) {...} print f(10);` leaks 177 **lists** alongside 38 strings, and the
  count tracks the interpreter's call count exactly (1 call → 1 list, 3 → 3,
  178 envs → 177). The strings are the `string`-field bug above; the lists are a
  separate, unresolved cause. Ruled out 2026-08-02, each with a clean minimal
  repro: a by-value `List<T>` parameter (with and without an enclosing
  try/catch + early return), `List<Struct>` where the struct owns a `Map`
  (at 180 elements, cross-module, and with a tagged-union value type),
  `.pop()` of a noncopyable element into a local, and exceptions generally.
  `List<Environment>` built from Lox's own type is also clean in isolation.
  Next step is to identify the allocation site rather than keep guessing shapes —
  a temporary alloc-site tag in the census, or bisecting `call_function`, which
  is where the per-call `args: List<LoxValue>` lives. May yet be a leak in the
  Lox *program's* own logic rather than a compiler bug; with no GC that is still
  a real leak, but it changes where the fix goes.

*Two `ref`-counting holes were fixed here on 2026-08-02 (both pre-existing,
both verified against an unmodified tree before the fix): a discarded
`ref`-returning call result leaked its handed-off count, and returning a `weak`
from any function segfaulted. Records in git history; the residual narrowing
question from the first is below.*

*Nine bugs were found and fixed here on 2026-08-02, all traced back to compiling
`CLAUDE.md`'s example program — which had never been run. Kept as a record of
what the test suite was not covering:*

*Five coroutine bugs, each unreachable until the one before it was fixed: the
DCE null-deref that made compiling any coroutine segfault (`values_by_id` left
null by `new_value()`, now `new_value_for(inst)` plus an `IRValidator`
invariant); an init function that hardcoded `param_is_ptr` to `false`, passing a
method receiver by value while the callee read a pointer; a stack receiver
reaching the state struct as a counted borrow, whose `RefInc` wrote through
`data - 8` into a neighbouring local (now `IROp::AssertHeap`, as closures do);
value structs in coroutine state storing an address into a field sized for the
struct (now inline, read by address with a `StructCopy` write-back); and
`out`/`inout` coroutine parameters, now a compile error rather than a pointer
into a dead frame.*

*Three destructor-chaining bugs: a child destructor failing to link when the
parent had none (chaining now targets the nearest ancestor that has one); an
inherited `uniq` field destroyed twice, once by the child and again by the
parent (each level now cleans only its own fields); and an inherited destructor
never running when the child declared none and had nothing to drop
(`struct_needs_synthetic_dtor` now treats an ancestor's destructor as an
obligation to carry the chain).*

*And one operator bug: an operator result was rejected as the left operand of
another operator, so `(a + b) * 2.0f` did not compile while `a.add(b).mul(2.0f)`
did.*

*`CLAUDE.md`'s example now compiles and runs verbatim.*

---

## Medium Priority

- [ ] **A discarded borrow is held to the end of its enclosing scope, not its
  statement**: `print(f"{box.borrow_item().v}"); delete box;` trips the
  free-trap, because the temporary borrow that the first statement created is
  only released when the scope closes. The count is no longer *leaked* (that was
  the High Priority item fixed 2026-08-02 — it is released, and per-iteration
  inside a loop body), so this is a lifetime-narrowing question rather than a
  leak, and it is the same rule every temporary follows: wrapping the use in an
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

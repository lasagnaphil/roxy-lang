# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned
improvements. Completed items are removed as they land — the per-item records
(measurements, rationale, regression-test pointers) live in this file's git history.

Last updated: 2026-07-17

---

## High Priority

(none currently)

---

## Medium Priority

Surfaced 2026-07-17 while making `examples/` compile again — each one is a rule
users hit immediately, and each was verified against the build.

- [ ] **`Printable` is unreachable except through f-string interpolation**:
  `print` is registered as `fun print(s: string)` (`natives.cpp` →
  `register_builtin_natives`), so `print(42)` is a type error ("cannot assign
  'i32' to 'string'") and every call site has to route through `f"{…}"`. The
  primitive `to_string` natives are `$$`-mangled (`i32$$to_string`, …), so
  neither `to_string(42)` nor `42.to_string()` resolves ("cannot access member
  of non-struct type") — interpolation is the only path to them. A struct that
  writes `fun T.to_string(): string for Printable` *can* call it directly, so
  the gap is primitives-only. Wants `print` to take a `Printable` and primitive
  `to_string` to be reachable by name. (`examples/hello.roxy` was `print(42)`
  and did not compile — it was cited as evidence that `print` accepts a literal.)
- [ ] **`List`/`Map` don't implement `Printable`**: `f"{items}"` fails with
  "type 'List<i32>' does not implement Printable (no to_string method)", so
  printing a container means a hand-rolled join loop (both showcases now carry
  one). Needs `to_string` on the generic native container types, with the
  element's own `Printable` impl threaded through monomorphization.
- [ ] **A trait bound isn't consulted for interpolation or operator dispatch**:
  a bound resolves an *explicit* method call — `<T: Eq>` makes `a.eq(b)`
  type-check in the body, which `examples/showcase.roxy` relies on — but the
  other two paths to a trait method don't ask it. With `<T: Printable>`,
  `f"{value}"` is rejected ("type 'T' does not implement Printable"), and with
  `<T: Ord>`, `a > b` is rejected ("invalid operands for comparison operator")
  even though `Ord` declares `gt`/`cmp` and operator dispatch is otherwise
  structural. So a bound is only half usable: the body has to spell out
  `value.to_string()` / `a.cmp(b)` instead. Verified 2026-07-17. Related to the
  `Printable` entry above — `to_string()` on a bounded `T` instantiated at a
  primitive also fails ("cannot access member of non-struct type").
- [ ] **No immutable borrow for containers**: a container parameter is either
  owning (`List<T>`, which *moves* the caller's value — `quicksort.roxy`'s
  `is_sorted(arr)` hit exactly this) or `inout`. `ref List<i32>` parses but
  won't bind ("cannot assign 'List<i32>' to 'ref List<i32>'"), and the grammar
  has no call-site `ref` (`argument -> ( "out" | "inout" )? expression`). So a
  function that only reads its argument must advertise mutation and take
  `inout`, and needlessly forbids aliasing it.

---

## Low Priority

- [ ] **Call depth is capped at 1024 frames with no way to raise it**:
  `VMConfig::max_call_depth` defaults to 1024 (`vm/vm.hpp`) and `roxy.cpp` never
  overrides it, so a recursive program deeper than ~1020 frames dies with
  "Call stack overflow" and the only recourse is rewriting it with an explicit
  stack. An embedder can set the config; a CLI user can't. A `--max-call-depth`
  flag (and the matching `--register-file-size`, currently 65536) would cost
  little. Verified 2026-07-17: `depth(1000)` returns, `depth(10000)` overflows.
- [ ] **Reading a missing `Map` key is a hard runtime error**: `m[k]` on an
  absent key aborts the program with "Map key not found" rather than yielding a
  default, so every read needs a `.contains()` guard (and a second lookup).
  There is no `get_or` / Option-shaped alternative. Verified 2026-07-17.
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
- [x] **Register overflow on huge single functions** — fixed 2026-07-17. Root
  cause (diagnosed with the structural generator's corpora): call results were
  bump-allocated at the frame top and never entered the active set, so every
  call permanently consumed its dst register plus its argument window — frames
  filled monotonically with dead call space, and `ensure_register_window` had
  no fallback at the 255 cliff. Now `reserve_call_window` places each call's
  dst+arg window at the lowest register above the live values (dead space is
  reused continuously; furthest-living values are spilled if even that doesn't
  fit), call results expire like other values (but are never *spilled* — the
  arg window is anchored at the dst register), and multi-register values are
  tracked in the active set (and excluded from spilling, since the spill
  bookkeeping is single-register).

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

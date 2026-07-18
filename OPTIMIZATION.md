# Roxy Compiler — Compile-Time Performance Program

This document is the working plan for optimizing **the compiler's own compile
time** (not the generated code and not the VM — for the IR optimization passes
the compiler applies to *user programs*, see `docs/internals/optimization.md`).
It records the measured baseline, the full findings of the 2026-07-17
whole-codebase study, the prioritized roadmap, and the measurement rules and
negative results that constrain future work.

Last updated: 2026-07-18. Baseline commit: `55d8fda`. **All of Tier 1 (§3) has
landed** — the §2 phase numbers predate those landings, so re-baseline
(`--repeat` + `sample`) before starting Tier-2 work.

---

## 1. Goals and methodology

The reference workload is `examples/lox/main.roxy` (~3,500 lines, 8 modules) —
the one reliably large, compiler-heavy program in the repo. All numbers below
come from:

```bash
cmake -B build-profile -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo \
      -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
ninja -C build-profile roxy
./build-profile/roxy --repeat=3000 examples/lox/main.roxy    # phase table
# sampling: run --repeat=15000 in background, then
/usr/bin/sample <pid> 20 1 -file profile.txt                 # leaf + call-graph
```

Full tooling reference: `docs/internals/profiling.md`. Never profile the
default `build/` (Debug, `-O0`, asserts live).

### Measurement discipline (non-negotiable)

- **Measure every change before committing.** One change at a time.
- The benchmark machine drifts thermally under sustained load, so for effects
  under a few percent, sequential before/after runs are unreliable. **Build
  both binaries and interleave runs** (stash → build → copy; pop → build →
  copy; alternate execution order across rounds).
- When an isolated phase timer says "win" but the interleaved **total** says
  neutral, trust the total (this exact discrepancy has happened — see §7).
- Instrumentation trick that has paid off: `atexit` counters on pass entry /
  no-op exits reveal which passes run without doing work (found two landed
  wins). Cheap, temporary, deleted before commit.

---

## 2. Measured baseline (2026-07-17, `55d8fda`, arm64 RelWithDebInfo)

### Phase breakdown (`--repeat=3000`, total 2.109 ms per compile)

| Phase       | Time     | Share  |
|-------------|----------|--------|
| ir-build    | 0.573 ms | 27.2%  |
| bc-lower    | 0.546 ms | 25.9%  |
| sema        | 0.352 ms | 16.7%  |
| ir-optimize | 0.303 ms | 14.3%  |
| parse       | 0.285 ms | 13.5%  |
| ir-validate | 0.045 ms |  2.1%  |
| topo-sort / link-other | ~0.006 ms | 0.3% |

### Leaf-level clusters (20 s `sample`, 16,489 leaf samples)

| Cluster | Share | Detail |
|---------|-------|--------|
| **Allocator traffic** (malloc/free/memmove/memset/bzero) | **~22%** | Top attributed drivers: `IRBuilder::emit_inst` 742 samples, `IRFunction::reorder_blocks_rpo` 379, `SymbolTable::define` 172, then a long tail across parser/lowering |
| Lexer (`next_token` + `skip_whitespace` + `identifier_type`) | ~8% | 732 + 400 + 166 samples |
| `compute_liveness` | ~5% | Still the #1 single function (855 samples) |
| `memcmp` | ~3% | Identifier-content comparisons: `find_variant` 61, `lookup_method_in_hierarchy` 43, `SymbolTable::lookup` 41, `find_variant_field` 40, `lookup_local` 36, `named_type_by_name` 36, `find_field` 34, … |

### Hot data-structure sizes

| Type | Size | Note |
|------|------|------|
| `IRInst` | 80 B | union inflated by 48 B call/closure variants (3× StringView/Span); individually bump-allocated, pointer-chased |
| `Expr` | 136 B | union inflated by inline `LambdaExpr` (~104 B); `Expr()` memsets 104 B per node |
| `Token` | 48 B | not stored en masse (parser has 1-token lookahead), so size is only a copy cost |
| `Type` | 176 B | interned + pointer-shared; size irrelevant |

### The three central architectural facts

1. **~22% of compile time is allocator traffic.** A large share traces to
   `Vector::ensure_capacity` (`include/roxy/core/vector.hpp:207`) doubling from
   capacity 1 — every small list walks the 1→2→4→8 realloc ladder — and to
   per-instruction bookkeeping in `emit_inst`.
2. **There is no compile-time identifier interning.** Every name is a raw
   `StringView` into the source; `std::hash<StringView>` is a byte-wise FNV-1a
   loop (`include/roxy/core/string_view.hpp:81-93`) re-run on **every** map
   lookup in sema, ir-build, and bc-lower, and every field/method/variant
   lookup memcmps string content.
3. **~68% of compile time (ir-build + ir-optimize + ir-validate + bc-lower)
   walks one data structure**: `Vector<IRInst*>` pointing at scattered 80-byte
   instructions interleaved in the bump arena with span/name allocations —
   poor locality, one pointer-chase per instruction per pass.

---

## 3. Tier 1 — quick wins (low risk, individually measurable)

> **Status: complete.** Every actionable item landed (§3.1, §3.2, §3.3, §3.4,
> §3.5, §3.7, §3.8, and §3.9's `detect_cycle`/`scan_imports`). §3.6 and §3.9's
> XXH3 swap were tried and reverted as negative results (§7). Next live work is
> Tier 2 (§4).

### 3.1 Gate `ir-validate` out of release compiles — guaranteed −2.1%

> **Landed (`9f4aca9`).** `link_modules()` gates the validator behind
> `#ifndef NDEBUG`; release/AOT compiles skip it (`compiler.cpp:429`). In
> release builds `ir-validate` now times 0.0 ms — the §2 baseline row below
> predates the gate.

`Compiler::link_modules()` runs `IRValidator::validate()` unconditionally
(`src/roxy/compiler/compiler.cpp:428-438`). The validator only checks
compiler-internal structural invariants (ValueId ranges, terminator presence,
jump-arg counts) — it catches compiler bugs, never user errors. Gate it behind
`#ifndef NDEBUG` or a `--verify-ir` flag; keep it on in debug, tests, and
fuzzing. Best impact-to-effort ratio in this document.

### 3.2 Lexer NUL sentinel — remove the per-byte bounds check

> **Landed (`b8f8d36`).** `peek()` is now an unchecked load
> (`src/roxy/shared/lexer.cpp:78-84`); the source buffer's NUL sentinel at
> `[m_length]` (asserted in the `Lexer` constructor) fails every scan-loop
> predicate, and `next_token()` keeps the single `is_at_end()` check.
> `peek_next()` retains its bounds check per the caveat below.

`peek()` (`src/roxy/shared/lexer.cpp:69-72`) branches on `is_at_end()` for
every source character; `skip_whitespace` (~3% of compile) is the hottest
consumer, plus the identifier/number/string scan loops.

**Verified safe:** every buffer entering the lexer is length+1 with
`buf[length] = '\0'` — `read_file` both overloads
(`src/roxy/core/file.cpp:36-42`, `:71-77`), native-signature parsing
(`src/roxy/vm/binding/registry.cpp:77-83`), and the LSP's `String::content`
(keeps a NUL, `include/roxy/core/string.hpp:53`).

Change: make `peek()` a plain load; `'\0'` already falls out of every scan
loop's predicate; keep exactly one `is_at_end()` in `next_token`. **Caveat:**
`peek_next()` (`lexer.cpp:74-77`) reads `m_current + 1`, which is one past the
allocation when sitting on the final NUL — keep its bounds check, or allocate
two trailing NULs. Add `assert(source[length] == '\0')` in the `Lexer`
constructor to lock the contract.

### 3.3 `m_block_offsets`: robin_map → direct-indexed vector (bc-lower)

> **Landed (`28d3526`).** `m_block_offsets` is now a `Vector<u32>`
> (`lowering.hpp:162`, sentinel `NO_OFFSET`), refilled per function via
> `clear_keep_capacity` — the same transformation that delivered −21% on
> bc-lower for the ValueId maps (`dbd74a6`).

`tsl::robin_map<u32,u32>` BlockId→code-offset (`lowering.hpp:141`), probed once
per Goto / twice per Branch in `patch_jumps` (`src/roxy/compiler/lowering.cpp:2717`)
plus per exception-handler/cleanup record. After `reorder_blocks_rpo`,
`block->id.id == array index` — keys are dense. Replace with a
`Vector<u32>` (sentinel `UINT32_MAX`), refilled per function via
`clear_keep_capacity` like `m_value_to_reg`. This is the same transformation
that delivered −21% on bc-lower for the ValueId maps (`dbd74a6`).

### 3.4 Sema: stop re-resolving the same symbol (call/identifier/get paths)

> **Landed (`d8e9fc2`).** The callee symbol is resolved once and threaded into
> the regular-call path; `check_not_moved`/`mark_moved`/`mark_live` gained
> `Symbol*` overloads so callers pass the symbol they already hold, and
> `analyze_get_expr` reuses the object lookup.

One ordinary call `foo(a, b)` today performs: `primitive_by_name` (a cascade of
up to 13 memcmps, `src/roxy/compiler/types.cpp:474-489`), `is_generic_fun` ×2
(`src/roxy/compiler/semantic.cpp:4078`/`4095` and again at `:2637`),
`m_symbols.lookup` ×3 (`:2648`, `:4050`, and a re-lookup inside
`check_not_moved`, `src/roxy/compiler/lifetime_checker.cpp:183`), and
`named_type_by_name` ×1 (`:4103`).

Change: resolve the callee symbol once at the top of `analyze_call_expr` and
thread it into `analyze_regular_fun_call`; add `Symbol*` overloads for
`check_not_moved` / `mark_moved` / `mark_live` so callers pass the symbol they
already hold; in `analyze_get_expr` (`:4302`) reuse the object lookup instead
of re-analyzing the identifier. Pure plumbing; preserve the lambda
capture-rewrite path in `analyze_identifier_expr`.

### 3.5 Sema: primitive operator dispatch table

> **Landed (`828724f`).** Dense `m_primitive_binary_ops[kind][op]` /
> `m_primitive_unary_ops` tables built once in `build_primitive_operator_tables`
> (`types.cpp:537`); per-use dispatch is `lookup_primitive_binary_op`
> (`types.cpp:562`), a pure array index. The struct/user-operator path still
> resolves by name and is subsumed by §5.1.

`i32 + i32` (and every comparison/bitwise/unary/compound op) resolves via
strings: `binary_op_to_trait_method(op)` → runtime `strlen` → `lookup_method`
→ `lookup_primitive_method` (`types.cpp:526-533`) = `robin_map<u8>` find +
**linear memcmp scan** over ~28 registered operator methods per primitive
(`src/roxy/compiler/trait_system.cpp:195-276`).

Change: build a dense `const MethodInfo* op_table[prim_kind][binary_op]` (and
a unary table) once in `register_primitive_operator_methods`, from the same
registrations (unregistered slot = nullptr → existing error path, so semantics
cannot diverge). Hot path becomes one array index. Also return `StringView`
from `binary_op_to_trait_method` to kill the per-call `strlen` on the
non-primitive path.

### 3.6 Sema: parameter/return types resolved twice — NEGATIVE RESULT (§7.5)

> **Attempted and reverted.** Writing `Param::resolved_type` in Pass 2 corrupts
> code generation: the IR builder prefers `param.resolved_type` over its own
> `type_by_name` fallback (`ir_builder.cpp:1137`), and method params previously
> relied on that fallback (`analyze_member_body` never wrote `resolved_type`).
> Feeding them the Pass-2 method-signature type diverges from the body-context
> type and produced a **double-delete** in the Lox interpreter test. Below-noise
> gain, real correctness hazard — do not re-attempt as specified. See §7.5.

Pass 2 resolves every signature (`register_fun_signature`,
`semantic.cpp:691` → `resolve_param_types`, `:1595`) and Pass 3 re-resolves
the same `TypeExpr`s to define body-scope symbols (`analyze_fun_body`,
`:1469`, return type `:1449`). Each `resolve_type_expr` (`:1200`) costs ~20
string comparisons + 1-2 map lookups. `Param::resolved_type`
(`include/roxy/compiler/ast.hpp:636`) already exists — write it in Pass 2,
read it in Pass 3. Same for methods/ctors/dtors.

### 3.7 IR-build: `gen_identifier_expr` triple lookup

> **Landed (`87f1585`).** The `LocalVar*` from `find_local` is hoisted and
> reused instead of re-walking the scope chain via `lookup_local`, and an
> `is_ptr` bit folded into `LocalVar` (set at param setup) retires the third
> (`m_param_is_ptr`) lookup.

The most frequent expression kind pays three lookups
(`src/roxy/compiler/ir_builder_expr.cpp:940`, `:1013`, `:1016`): `find_local`
(walks the scope chain, already yields the `LocalVar` holding value + type),
then `lookup_local` (re-walks the entire chain for the same value), then
`m_param_is_ptr.count` (third hash lookup). Hoist the `LocalVar*` and reuse
it; fold an `is_ptr` bit into `LocalVar` (set at param setup) so the third
lookup disappears. Subsumed by §5.3 if that lands, but worth taking now.

### 3.8 bc-lower: `m_const_skip_load` robin_set → dense bitvector

> **Landed (`3a90eb2`).** `m_const_skip_load` is gone; skip-load eligibility is
> derived directly from the dense `requires_register` vector
> (`!requires_register[id]` for a numeric const), which also deleted the second
> collection pass.

A `robin_set<u32>` of ValueIds (`lowering.hpp:234`) probed **twice per
result-producing instruction** (`lowering.cpp:236`, `:1750`). The information
is derivable from the dense `requires_register` vector that
`compute_const_use_modes` already builds (`:963`) — a numeric const is
skip-load-eligible iff `!requires_register[id]`. Keeping it dense also deletes
the second collection pass (`:1135-1145`).

### 3.9 Small free cleanups

- **`detect_cycle` rebuilds `name_to_idx` on every recursive call**
  (`compiler.cpp:156-159`) — the outer `topological_sort` already built it
  (`:131-134`). Pass it by reference. O(V·(V+E)) → O(V+E); negligible at 8
  modules but a pure defect.
- **`scan_imports` re-lexes every module in full** before compile
  (`src/roxy.cpp:173-203`) just to read import names. It runs before the
  `--repeat` loop, so it is **invisible in every profile**, but it roughly
  doubles lexer work for real one-shot compiles. Imports are top-of-file:
  stop at the first non-`import`/`from` top-level token.
- **`std::hash<StringView>`: FNV byte loop → XXH3** (`string_view.hpp:81-93`).
  The vendored XXH3 is already trusted for the CSE key
  (`ir_optimize.cpp:578`) and is well-avalanched (robin_map buckets on low
  bits). A cheap, broad 1-3% — but a strict subset of §5.1; skip it if
  interning is imminent. **Tried and reverted (2026-07-17):** measured
  perf-neutral on Lox — XXH3 must be out-of-line (a `.cpp`) to keep the
  6.7k-line `xxhash.h` out of the ubiquitous header, and the call overhead
  offsets XXH3's better distribution on short identifiers. Worse, defining it
  in `roxy_core` **broke AOT linkage**: the C backend links the *standalone*
  `roxy_rt` runtime, which pulls in `std::hash<StringView>` but can't see a
  `roxy_core` symbol → `undefined symbol rx::hash_string_view_bytes`. An inline
  version would fix linkage but bloats the compiler's own build for a neutral
  runtime result. Leave FNV; let §5.1 interning retire StringView hashing
  entirely. (`detect_cycle` and `scan_imports` above both landed.)

---

## 4. Tier 2 — medium efforts

### 4.1 Fuse `compute_liveness` passes 1-3 into one walk (bc-lower)

> **Landed (`aae5373`).** `compute_liveness` folded its first three full-IR
> walks into one: Pass 1 (def points), Pass 2 (operand last-uses), and Pass 3
> (extend block params to predecessor terminators) share one program-point
> numbering and are independent / order-tolerant (`mark_use` is a max). Pass 4
> (back-edge extension) stays separate — it reads finalized ranges. Folding
> Pass 3 into the forward walk is sound: a forward target's param extension is a
> no-op (its def_point exceeds the terminator point) overwritten by the later
> def; a back-edge target is already defined earlier in the walk, so its
> extension lands — byte-identical live ranges. Measured **bc-lower −1.8%**,
> total −0.5% (5/5 rounds negative) on corpus_400. VM suite green (1394 cases).

Passes 1 (def points, `lowering.cpp:1157-1175`), 2 (uses, `:1177-1337`), and
3 (block-param extension at predecessor terminators, `:1339-1379`) are three
sequential full-IR walks. `def_point` and `last_use` are independent fields,
`mark_use` is an order-independent max, and Pass 2 never reads `def_point`, so
one forward walk can do all three (Pass 3's terminator point is known during
the same walk). Pass 4 (back-edge extension) must stay separate — it reads
finalized `last_use`. Note: Pass 4 (`:1417-1450`) is O(back_edges × values), a
latent quadratic on loop-heavy functions; not currently hot, worth watching.

### 4.2 Register allocator: kill the O(active²) and the linear min-scans

> **Landed (`de54bc5`).** Both parts done. `expire_before` now counts the
> sorted-prefix of dying entries and shifts the surviving tail once (O(active)
> instead of O(prefix·active)). The `m_free_regs` `Vector<u8>` became a 256-bit
> mask (`m_free_mask[4]`): "smallest free register" is `std::countr_zero` over
> four words, freeing a register is one bit-set, and the three duplicated
> min-scans collapse to `free_reg_take_min()`. **Measured −18.3% on bc-lower**
> (77.0 → 62.9 ms on corpus_400), ≈ −2.6% total — the largest single bc-lower
> win since `dbd74a6`. Verified: full VM test suite green (1394 cases).

- `expire_before` (`lowering.cpp:1532-1547`): each front-pop shifts every
  remaining `m_active` element left, so expiring k entries is O(active²), once
  per program point. The expiring entries are a sorted prefix — count the
  prefix, free those registers, then do one memmove. O(active) per call.
- "Smallest free register" is a full linear scan of `m_free_regs`, duplicated
  three times (`lowering.cpp:736-744`, `:750-758`, `:766-774`). Replace the
  free list with a fixed 256-bit bitset member (4× u64): find-min =
  count-trailing-zeros, alloc/free = bit ops. A fixed member, not a per-pass
  scratch buffer, so it does not fall under the scratch-reuse negative result.

### 4.3 ir-optimize: compute predecessors once per fixed-point iteration

> **Landed (`ecfa693`).** `optimize_function` builds the `PredecessorMap`
> once per outer iteration (after branch folding settles the edges) and threads
> it through both `run_block_merging` — which refreshes it internally only after
> a pass that actually merged, and leaves it valid on return — and
> `run_trivial_block_arg_elim`, which never touches edges. The common no-op
> iteration now builds the map once instead of once per pass. Measured
> **ir-optimize −0.9%** (−0.6 ms on corpus_400), total −0.25% — small but
> consistent (5/5 rounds negative). VM suite green (1394 cases).

Each iteration builds the CSR `PredecessorMap` twice — inside
`run_block_merging`'s loop (`ir_optimize.cpp:303`) and again at the top of
`run_trivial_block_arg_elim` (`:417`) — although trivial-arg-elim was measured
**193/193 no-op** on Lox and block-merging usually changes nothing. Compute it
once per iteration in `optimize_function`, thread it into both passes,
recompute only when block-merging reports `changed` (the pass already
tolerates stale entries per its own comment at `:289-300`).

### 4.4 Parser: switch-based dispatch

> **Attempted — measured neutral, reverted (§7.7).** Converting `declaration()`,
> `statement()`, and `expression()`'s `match()`/`check()` chains to `switch`es on
> `m_current.kind` left **parse +0.10%** (the phase it targets — dead neutral)
> and total +0.16%, both inside the noise band, on corpus_400. The dispatch
> chains are highly branch-predictable, so the CPU predictor already collapses
> them and a jump table wins nothing at N≈11–19 arms. Tests green; not committed.

An ordinary expression statement runs ~7 failed `match()` calls in
`declaration()` (`src/roxy/compiler/parser.cpp:1426-1486`) plus ~12 more in
`statement()` (`:968-1006`) — ~19 sequential kind-compares per statement —and
`expression()` (`:187-192`) tests an 11-way chained `check()` for assignment
operators on every expression. Convert both to a single `switch` on
`m_current.kind` (keep the `pub`/`native` prefix handling ahead of it).

### 4.5 Parser: stop allocating + copying a `Stmt` per declaration

> **Attempted — measured neutral, reverted (§7.8).** Built the common expression
> statement directly into `declaration()`'s wrapping `Decl` (threaded `dest`),
> skipping a separate `alloc<Stmt>()` and the 88-byte `decl->stmt = *stmt` copy.
> Across three interleaved corpus_400 runs, parse's delta tracked the *untouched*
> ir-build / bc-lower phases (a ~−0.7% systematic thermal offset) — no
> parse-specific signal. A *bump* `alloc<T>()` is a pointer increment (~10 ns),
> so eliminating one plus a small copy is below the noise floor. Tests green; not
> committed.

`declaration()` (`parser.cpp:1486-1493`, also `for_statement` `:1099-1106`)
arena-allocates a `Stmt`, then allocates a `Decl` and **byte-copies the whole
Stmt into the Decl's union**, abandoning the original — one wasted allocation
plus an ~80-byte copy for the bulk of function-body statements. Add a
build-in-place variant that fills `decl->stmt` directly; keep the
`Stmt*`-returning form for `if`/`while`/`for` bodies.

### 4.6 IR-build: memoize `collect_assigned_vars`

> **Attempted — measured a regression, reverted (§7.9).** Memoizing each compound
> statement's assigned-var set in a per-function
> `robin_map<Stmt*, Vector<StringView>>` (cleared in `begin_function_body`) made
> **ir-build +2.6% slower** (122.9 → 126.0 ms) on corpus_400. Correct
> (phi/block-arg order preserved, VM suite green), but at this corpus's nesting
> depth (~3–5) the re-walk savings don't recover the cost of a per-compound-stmt
> heap `Vector` (one each across ~18K ifs + ~13K loops). Not committed.

Each `gen_if`/`gen_when`/`gen_while`/`gen_for`/`gen_try` calls
`collect_assigned_vars` (`src/roxy/compiler/ir_builder_stmt.cpp:1335-1484`),
which recurses through the entire nested statement subtree — so a statement
nested under k control-flow constructs is re-walked k times (super-linear in
nesting depth), each call also constructing a fresh `robin_map` seed. Compute
assigned-var sets bottom-up once, memoized per `Stmt*`; preserve
first-occurrence ordering (phi param order depends on it) and the inout-arg
write handling (`:1460-1463`).

### 4.7 Operand-shape LUT: unify three hand-synced ~90-way switches

> **Deferred (not attempted).** After the Tier-2 pass, this was the one item left
> unimplemented — deliberately. Its stated value is **maintainability** (collapse
> three hand-synced ~90-way IROp switches into one `operand_shape[256]` table),
> not compile time, and every dispatch-style change measured this pass (§4.4) was
> perf-neutral, so it is unlikely to move the metric this program targets. It is
> also the **highest miscompile risk** in Tier 2: the table feeds
> `for_each_operand` → liveness → register allocation, so a single wrong entry
> among the ~90 IROps could miscompile in a way the suite might not catch (cf. the
> §7.5 double-delete that passed all *compiler* tests). Worth doing as a focused,
> separately-reviewed **code-quality** change with the `static_assert`-against-
> enum-count guard the plan describes — but out of scope for a compile-time pass.

`for_each_operand` (`include/roxy/compiler/ir_optimize.hpp:131-252`),
`compute_liveness` Pass 2 (`lowering.cpp:1185-1305`), and
`compute_const_use_modes` (`:973-1100`) are three near-duplicate giant
switches over all IROps — and operand enumeration is the innermost operation
of both phases. In `IRInst`'s union, the first operand sits at offset 0 and
the second at offset 4 for nearly every op (common-initial-sequence). Add
`static const u8 operand_shape[256]` classifying ops as NONE / ONE / TWO /
COMPLEX (calls, New, Closure, SetField-with-store, IndexSet → the existing
cold switch), rewrite `for_each_operand` on top of it, and make the other two
call it. `static_assert` the table against the enum count.
Prototype-and-measure — but even at perf-neutral it collapses three switches
that must currently be kept in sync by hand (`ir_optimize.hpp:129`).
Note: `compute_const_use_modes` needs LHS/RHS position awareness for RK ops
(`:981-1004`), so it needs a position-aware variant of the iterator.

### 4.8 Parser: reserve transient list vectors

> **Landed (`c90994b`).** `reserve(8)` on the three highest-frequency transient
> parser Vectors — call arguments, function parameters, and block declarations —
> collapses the core `Vector` 1→2→4→8 malloc/realloc/free ladder to a single
> allocation. Each reserve is guarded by the same non-empty check that gates the
> parse loop, so zero-arg calls and empty blocks stay allocation-free (no
> pessimization of the empty case). Measured **parse −4.4%** (55.9 → 53.4 ms on
> corpus_400), ≈ −0.5% total. VM suite green. The remaining lower-frequency sites
> (struct/enum fields, f-string parts, when cases) are available for incremental
> gains. **This is the item that overturns the §4.4/§4.5 "parse has no headroom"
> read** — the headroom is in the core-`Vector` realloc ladder (real `malloc`
> traffic), not in dispatch or bump-allocation.

Core `Vector` grows 1→2→4→8 (`vector.hpp:207-217`), so every parsed list
(call args, params, block statements, fields, variants — all built in a
transient `Vector` then copied to the arena via `alloc_span`) pays ~log2(N)
malloc/free/move cycles. `reserve(8)` (or a SmallVector with inline storage)
**at the parser call sites only** — do not change core `Vector`'s policy
globally without interleaved measurement, since ir-build/bc-lower containers
have different size distributions. This is an allocation-*count* reduction,
not the reuse pattern that measured slower (§7).

### 4.9 Box `LambdaExpr` out of the `Expr` union

> **Attempted — neutral on compile time, reverted (§7.10).** Boxing `LambdaExpr`
> to a `LambdaExpr*` shrank `sizeof(Expr)` 136 → 112 B and the per-node `Expr()`
> memset 104 → 80 B, but measured neutral on corpus_400 (parse +0.31%, total
> +0.24% — all phases inside noise): 24 B less memset per node is too small a
> slice of per-node work to register, and denser nodes don't speed the
> pointer-chasing AST walks. The change is a real **18% AST-node memory
> reduction** and a clean design (a rare 104 B variant no longer sizes the hot
> union), so it may be worth keeping for *footprint* — but that's out of scope
> for a compile-*time* program. Not committed here.

`sizeof(Expr)` is 136 B driven by the inline `LambdaExpr`
(`ast.hpp:354-368`), and `Expr()` memsets 104 B on every node
(`ast.hpp:492-498`) — paid by the most numerous nodes (literals, identifiers,
binaries). Store `LambdaExpr*` in the union (lambdas are rare; allocate on
parse at `parser.cpp:489`): `Expr` drops to ~112 B (next-largest variant is
`CallExpr`), memset to ~80 B, and every later AST walk gets denser nodes.
Touches every `expr->lambda.*` reader in sema/IR-build.

---

## 5. Tier 3 — architectural bets (highest ceiling)

### 5.1 Intern identifiers to `u32` symbol IDs at lex time — ⛔ ABANDONED (§7.6)

> **Implemented, measured, and reverted — a net regression. See §7.6.** The
> design and the full findings live in `docs/internals/identifier-interning.md`
> (retained as a record of what was built and why it didn't pay). Bottom line:
> interning at lex is a **+5.6% total** regression on the 400-module corpus
> (parse +26.7%), the cost is the structural per-token hash+probe (not the byte
> copy), and the §5.2b `IRInst` shrink it was meant to unblock can't help because
> **§5.2a proved the IR walk is not `IRInst`-locality-bound**. Only Phase P (the
> canonical mangler) was kept. The optimistic description below is preserved for
> context but the premise did not hold.

**The single highest-ceiling change.** Today every keyed lookup in sema,
ir-build, and bc-lower re-hashes identifier bytes (FNV-1a) and every probe
memcmps; every field/method/variant/trait resolution
(`find_field`, `find_variant`, `lookup_method_in_hierarchy`, …) is a linear
string scan; `resolve_type_expr` does `== "Self"/"List"/"Map"/"Coro"` string
compares plus the 13-memcmp `primitive_by_name` cascade on every named type.

Change: an intern table (lexer- or Compiler-owned) mapping byte range →
`Sym{u32}` populated once per unique identifier at lex time; a side table
`Sym → StringView` for diagnostics and codegen. Name fields in Token/AST/IR
become 4 bytes; all `robin_map<StringView, …>` become `robin_map<Sym, …>`
(hash = identity, equality = int compare), and the small linear scans become
integer compares. Keyword strings (`"Self"`, `"List"`, primitive names) become
pre-interned constants compared by ID.

Estimated impact: 5-15% of total compile (kills the ~3% memcmp cluster, all
FNV hashing above it, and compounds §5.2's size reduction). Blast radius is
wide — Token, AST, IR, LSP, and the `intern_format` mangled-name world must
agree — so migrate behind a typedef with both representations coexisting.
Cheap down-payment if deferred: the XXH3 hash swap (§3.9).

### 5.2 `IRInst`: contiguous pool + shrunken union — 5.2a tried, NEUTRAL (§7.6)

Two composable steps:

- **5.2a — contiguous pool: TRIED AND REVERTED, measured neutral (§7.6).**
  Allocated `IRInst`s from a dedicated Compiler-owned pool (via an optional
  `IRBuilder` ctor arg) instead of interleaving them with span/name allocations
  in the shared bump arena (`ir_builder_expr.cpp:22-33`). On the 400-module
  corpus this was **neutral-to-slightly-worse** across ir-build / ir-optimize /
  bc-lower — **the IR-walking phases are not `IRInst`-cache-locality-bound.** The
  real cost in those phases is the `Vector<IRInst*>` pointer indirection and the
  per-op compute, not the locality of the targets. This also kills the rationale
  for 5.2b (below): a *smaller* `IRInst` can't speed up a walk that isn't
  locality-bound, and `BumpAllocator` is per-alloc (size-independent) so there is
  no allocation-time saving. If IR-walk cost is ever attacked, target the
  indirection (e.g. inline small instructions, or SoA the hot fields), not
  `IRInst` size/locality.
- **5.2b — shrink the union (needs §5.1): NOT PURSUED — predicted neutral by
  the 5.2a result.** The union is sized by `CallExternalData`/`ClosureData` (3×
  16-byte StringView/Span, `ssa_ir.hpp:225-247`); with names as `u32` these drop
  to ~24 B and `IRInst` goes 80 → ~56 B. But its only benefit is locality on the
  IR walk, which 5.2a showed is not the bottleneck — so a ~65-site change across
  lowering + `c_emitter` (miscompile-prone, all-or-nothing to measure) for a
  predicted-neutral result. Not worth it, and it depends on the abandoned §5.1.

### 5.3 IR-build scope environment: flat slot array

Today: `Vector<tsl::robin_map<StringView, LocalVar>> m_local_scopes`
(`include/roxy/compiler/ir_builder.hpp:556`). `define_local` walks all scopes;
`lookup_local`/`find_local` walk innermost→outermost with a hash per level;
and `snapshot_scopes`/`restore_scopes`
(`ir_builder_stmt.cpp:133-154`) **deep-copy the entire map chain** —
`gen_when` does N+2 deep copies for N cases (`:874`), `gen_if_stmt`
snapshots unconditionally (`:251`) even when never restored. This is the
~2.4%-of-compile item where the undo log failed (§7).

Key enabler: **local shadowing is banned** (semantic check) and temps get
unique names, so at most one live binding per name exists. Change:

- one function-lifetime `robin_map<StringView (or Sym), u32> name_to_slot`,
  populated lazily, **never copied**;
- current bindings in a flat, trivially-copyable `Vector<LocalVar> m_slots`;
- `define_local` = 1 hash + array write; `lookup_local` = 1 hash + array read
  (**fewer** hashes than today's multi-scope walk — the hot path gets faster,
  which is exactly why this succeeds where the undo log failed);
- snapshot = POD memcpy of `m_slots`; restore = memcpy back.

Scope-exit cleanup stays the OwnershipTracker's job (unchanged). Watch-outs:
`find_local` pointer stability across growth; yield's iterate-live-locals loop
(`ir_builder_stmt.cpp:965-971`) needs a live-slot list instead of iterating
scope maps. Keep the existing `snapshot_scopes`/`define_local`/`lookup_local`
API so call sites don't change. Estimated 5-10% of ir-build; subsumes §3.7
and the `gen_if` unconditional-snapshot waste.

### 5.4 SoA token buffer

Lex each module once, up front, into structure-of-arrays storage:
`kind[]` as `u8` (give `TokenKind` a fixed underlying type; ~90 kinds),
parallel `offset[]`/`length[]` touched only when building AST nodes. Then:

- `advance()` = index increment (no 48-byte Token copies);
  `check()`/`match()` stream a dense 1-byte array;
- `save_state`/`restore_state` = save/restore a `u32` index — **generic
  trial-parse backtracking becomes free** (today it re-lexes);
- the lexer becomes one tight loop (best home for the NUL sentinel).

Cost: one upfront allocation per module (~30k tokens for the workload); the
`>>`/`>>=` in-place token-splitting hack in `consume_closing_angle`
(`parser.cpp:2239-2311`) must be reworked to an index/sub-offset scheme.
Medium-large refactor; the one change that moves the lexer ~8% and
parser-proper ~8% together.

### 5.5 Parallelism (parse-only first; a scaling investment)

Full sema parallelism is blocked by genuinely shared mutable state: the single
`BumpAllocator` (`bump_allocator.hpp:40-70`, not thread-safe), the `TypeCache`
intern map (`types.hpp:561`), `ModuleRegistry` exports, and the
cross-module generic queues (`compiler.cpp:281-320`). At 2.1 ms total, thread
overhead eats most of the win. The safe slice is **parse**: per-source,
embarrassingly parallel, writes only `m_module_states[i]`
(`compiler.cpp:99-126`) — up to ~14% wall-clock at perfect scaling,
realistically less. Worth doing only when project sizes grow; behind a
thread-count flag.

---

## 6. Landed optimizations (history)

All measured on Lox, RelWithDebInfo, arm64, interleaved where the effect was
small. Cumulative through `55d8fda`: total compile 2.545 → ~2.11 ms (≈ −17%);
bc-lower 0.781 → 0.533 ms (−32%).

| Commit | Change | Effect |
|--------|--------|--------|
| `dbd74a6` | BytecodeBuilder ValueId robin_maps → direct-indexed vectors | bc-lower −21%, total −7.1% |
| `e9d5276` | compute_liveness same-block check → binary search | bc-lower −12%, total −4.5% |
| `788a2bf` | run_local_cse: hoist per-block CSEKey map out of loop | ir-optimize −7.1% |
| `9409841` | compute_predecessors → CSR (flat offsets + edges) | ir-optimize −15% |
| `c9a40df` | compute_liveness Pass 4 fixed point → one ordered sweep | bc-lower −2.2% |
| `41e9797` | intern_synthetic_name/intern_concat fast paths (no printf formatter) | ir-build −5.8% |
| `8c8b706` | trivial_block_arg_elim: defer allocs in no-op path | ir-optimize −4.8% |
| `55d8fda` | local_cse: defer subst alloc; copy-prop: drop zero-fill | ir-optimize −7% |
| `828724f` | §3.5 dense primitive operator-dispatch tables (name-free) | sema |
| `9f4aca9` | §3.1 gate ir-validate behind NDEBUG (skip in release) | −2.1% (release) |
| `b8f8d36` | §3.2 lexer `peek()` unchecked load via NUL sentinel | lexer |
| `28d3526` | §3.3 direct-index `m_block_offsets` instead of a robin_map | bc-lower |
| `d8e9fc2` | §3.4 reuse resolved symbols instead of re-looking them up | sema |
| `87f1585` | §3.7 collapse `gen_identifier_expr`'s triple lookup | ir-build |
| `3a90eb2` | §3.8 derive const skip-load from dense requires-register flags | bc-lower |
| `4248e33` | §3.9 free cleanups (`detect_cycle` map, `scan_imports` early-out) | compiler |
| `de54bc5` | §4.2 bitset free-register set + O(active) `expire_before` | bc-lower −18.3% |
| `ecfa693` | §4.3 share one `PredecessorMap` across block-merge + trivial-arg-elim | ir-optimize −0.9% |
| `aae5373` | §4.1 fuse `compute_liveness` passes 1-3 into one IR walk | bc-lower −1.8% |
| `c90994b` | §4.8 `reserve(8)` the hot transient parser Vectors (guarded) | parse −4.4% |

---

## 7. Negative results — do NOT retry

These were implemented, tested green, measured, and **reverted**. The pattern:
on macOS, same-sized transient `Vector` mallocs are served from a hot
free-list in ~tens of nanoseconds; replacing them with reuse schemes costs
more than it saves. "Reduce allocations" only pays where allocation is
genuinely expensive (robin_map bucket rehashes, nested variable-sized
`Vector<Vector>`) or where it removes an allocation *class* entirely.

1. **Scratch-buffer reuse across ir-optimize passes** (shared reusable
   subst/use_counts/is_dead/worklist buffers): ~1% *slower*. The reuse reset
   cost more per element than the malloc it saved. Corollary: an arena is
   also wrong there (`BumpAllocator` never frees until destruction; `Vector`
   is not allocator-aware).
2. **Eliminating the redundant `rpo_order` Vector in `reorder_blocks_rpo`**
   (reverse-index `post_order`): neutral-to-slower; reverse indexing is
   marginally worse for cache than the sequential copy.
3. **Pre-reserving `BCFunction::code`** in build_function: isolated phase
   timer said −3%, interleaved **total** was neutral. Trust total.
4. **ScopeSnapshot undo log** in the IR builder (mark/rollback replacing
   gen_when's deep copies): mechanism correct, all tests passed, ~0.01 ms
   *slower* per compile — scope maps are small so the deep copy was already
   cheap, and the log taxed the extremely hot `define_local` path. A ~2.4%
   profile line is not automatically beatable; a cheap-per-op ×many-ops cost
   resists replacement by another cheap-per-op ×many-ops scheme. (The flat
   slot array of §5.3 is the structural alternative that makes the hot path
   *faster* instead of taxing it.)
5. **Caching resolved param types on `Param::resolved_type` (§3.6)** — write in
   Pass 2's `resolve_param_types`, read in Pass 3's body analyzers. Tests green
   for the *compiler*, but the Lox interpreter E2E **double-deleted** at runtime.
   Root cause: the IR builder prefers `param.resolved_type` over its own
   `type_by_name`+`apply_ref_kind` fallback (`ir_builder.cpp:1137-1146`), and
   method params previously relied on that fallback — `analyze_member_body` never
   wrote `resolved_type`, so it stayed null. `resolve_param_types` is shared by
   functions *and* methods/ctors/dtors, so writing it there fed method params a
   type resolved in the signature-registration context (not the struct-scope body
   context), diverging from the fallback and mis-deriving the param's drop plan.
   The gain is below measurement noise (a handful of `resolve_type_expr` calls
   per function, like §3.4); not worth auditing every reader of the field to make
   it safe. If retried: the field is a shared sema→IR-builder contract, so any
   caching must produce *exactly* the type the IR-builder fallback would, for
   every member kind, or route methods through a separate field.
6. **Identifier interning to `u32` Syms at lex time (§5.1) + `IRInst`
   contiguous pool (§5.2a).** Built through Phase 0/1a/1b/1c/1d + §5.2a, measured
   on the 400-module / 257 KLOC corpus (the reliable workload — Lox at ~2 ms is
   below the noise floor and gave misleading sub-2% "wins"), then **reverted**.
   Interning was **+5.6% total** (parse +26.7%, sema +4.3%): the cost is the
   per-identifier-token **hash + robin_map probe at lex**, paid on far more tokens
   than the lookups it saves. It is **structural** — a no-copy `intern_stable`
   recovered only ~6% of the parse tax, proving the cost is the hash/probe, not
   the byte copy. The intended payoff (§5.2b `IRInst` shrink → IR-walk locality)
   evaporated when **§5.2a measured neutral**: the IR walk is not
   `IRInst`-cache-locality-bound (the bottleneck is `Vector<IRInst*>` indirection
   + per-op compute), so a smaller `IRInst` cannot help. **Kept only Phase P** (the
   canonical mangler, `compiler/mangling.{hpp,cpp}`) — perf-neutral, and it
   eliminated a real 8-way `$$`-mangling drift hazard independent of interning.
   Methodology lesson: **measure Tier-3 bets on the large `roxy_gen` corpus, not
   Lox** — every Phase-1 sub-slice looked like a ~1-2% "win" on Lox and was
   actually noise masking a cumulative regression. Full write-up:
   `docs/internals/identifier-interning.md`.
7. **Parser switch-based dispatch (§4.4)** — replaced the sequential
   `match()`/`check()` kind-compare chains in `declaration()` / `statement()` /
   `expression()` with `switch`es on `m_current.kind` (jump table vs. up to ~19
   sequential compares per statement, 11 per expression). Measured on
   corpus_400: **parse +0.10%** — the only phase touched, i.e. dead neutral —
   and total +0.16% ± 0.21% (within noise, if anything marginally worse). The
   dispatch chains are highly branch-predictable, so the CPU's branch predictor
   already collapses them; a jump table saves nothing at N≈11–19 arms. Tests
   stayed green; reverted as a perf-neutral change. (The `expression()` arm did
   remove a genuine redundancy — an 11-way `check()` chain immediately mirrored
   by an identical mapping `switch` — worth revisiting as a pure readability
   cleanup, not a perf change.)
8. **Build the Decl's statement in place (§4.5)** — the common expression
   statement was built directly into `declaration()`'s wrapping `Decl` (via a
   threaded `dest`), skipping a separate `alloc<Stmt>()` and the 88-byte
   `decl->stmt = *stmt` copy. Measured neutral on corpus_400 across three
   interleaved runs (one thermally clean): parse's delta tracked the *untouched*
   ir-build / bc-lower phases (~−0.7% systematic thermal offset), so there was
   no parse-specific signal. The reason generalizes this section's opening
   lesson to the *bump* allocator: a bump `alloc<T>()` is a pointer increment
   (~10 ns), so removing one allocation + a small copy sits below the noise
   floor. Tests green; reverted. **Meta-finding:** two parse-targeted micro-opts
   in a row (§4.4, §4.5) measured neutral — parse (~15% of compile) has little
   headroom in per-node *dispatch* (branch-predictable) or *bump*-allocation
   (pointer-bump cheap). **Corrected by §4.8:** parse *does* have real headroom
   in the core-`Vector` 1→2→4→8 malloc/realloc ladder for transient lists —
   `reserve(8)` there measured **parse −4.4%**. The lesson is narrower than
   "parse is tapped out": micro-opts that only touch dispatch or bump-allocation
   don't pay, but cutting genuine `malloc` traffic does.
9. **Memoize collect_assigned_vars per compound Stmt* (§4.6)** — cached each
   block/if/while/for/when/try subtree's ordered assigned-var set in a
   per-function `robin_map<Stmt*, Vector<StringView>>`, cleared in
   `begin_function_body`, to kill the super-linear re-walk (a statement nested
   under k control-flow constructs is collected k times). Correct — phi/block-arg
   order preserved, full VM suite green — but **ir-build +2.6%** (122.9 → 126.0 ms)
   on corpus_400, a regression. The cache's per-entry heap `Vector` (one per
   compound statement — ~18K ifs + ~13K loops in the corpus) plus the map hashing
   cost more than the re-walks they eliminate at this corpus's nesting depth
   (~3–5). Same lesson as items 1 and 8: memoization pays only when the recomputed
   work is expensive relative to a heap allocation, and a shallow-nesting re-walk
   of `StringView`s is not. If revisited, the cache must be allocation-free — e.g.
   a single flat `StringView` arena with an `(offset, count)` per `Stmt` in the
   map — to have a chance; the per-entry-`Vector` form is a measured loss.
10. **Box LambdaExpr out of the Expr union (§4.9)** — replaced the inline
    `LambdaExpr lambda` (104 B) with a `LambdaExpr*`, dropping `sizeof(Expr)`
    136 → 112 B and the per-node `Expr()` memset 104 → 80 B. Small, clean change
    (5 sites: the union member, the parser creation, two `LambdaExpr&` deref
    bindings in sema/ir-build, and the generics clone's fresh allocation —
    `emplace` value-initializes, so the analysis fields stay zeroed as the old
    memset left them). Full VM suite green. But **neutral on compile time**
    (parse +0.31%, total +0.24% ± 0.80% on corpus_400): the 24-B-smaller memset
    is a negligible slice of Expr creation, and the smaller node doesn't speed
    the pointer-chasing AST walks (112 B still spans two cache lines, and nodes
    aren't visited in allocation order). Reverted for this program. **Caveat /
    possible keep:** it is a genuine 18% AST-node memory reduction with a clean
    design (a rare fat variant no longer sizes the hot `Expr` union) — worth
    resurrecting as a memory-footprint change, which this compile-time-scoped
    program does not measure.

---

## 8. Verified already-fine — do not re-investigate

- **Type interning / comparison**: compound types hash-cons on component
  *pointers* (`types.cpp:169-272`); hot-path equality is `==`.
- **SymbolTable**: O(1) lookups via `m_lookup_cache` with shadow-restore on
  `pop_scope` — no linear scope scans. Remaining cost is the FNV hash of the
  key, not the structure (→ §5.1).
- **Module system**: each module parsed exactly once, analyzed exactly once;
  imports resolved by reading registry exports, never re-analysis. The
  cross-module generic drain loop is real work, not redundancy.
- **Lexing is already fused into parsing** — one-token lookahead, no token
  array materialized in the compile path (until/unless §5.4).
- **Keyword trie** (`identifier_type`) is optimal.
- **Phi/block-arg construction**: structural, O(vars) per merge, no
  fixed-point iteration — the cost is upstream in `collect_assigned_vars`
  (§4.6), not the phi machinery.
- **`robin_map` default construction does not allocate** (static empty
  bucket), so empty scope pushes are free.
- **bc-lower side tables** (`m_value_to_reg`, `m_value_types`) already dense
  direct-indexed vectors; `m_reg_to_value` a fixed 256-array.
- **Liveness cannot be computed once and shared** ir-optimize → bc-lower: the
  optimizer mutates the CFG, invalidating it; the phases also need different
  analyses (use counts vs live ranges).
- **`reorder_blocks_rpo`'s build-side call is not removable**: coro-lower
  consumes the builder's RPO output (no reorder between build and coro-lower);
  removing it just moves the same cost out of the ir-build bucket.
- **`get_or_add_int/float_constant`** linear scans: bounded (<100
  entries/function); not worth a map.
- **Expr/Stmt single-shot analysis**: bodies are asserted analyzed exactly
  once; no accidental re-walks in sema.

---

## 9. Suggested execution order

1. ~~Tier 1 (§3) — fully landed~~ (`9f4aca9`, `b8f8d36`, `28d3526`, `d8e9fc2`,
   `828724f`, `87f1585`, `3a90eb2`, `4248e33`); §3.6 and §3.9's XXH3 swap were
   reverted (§7). **Re-baseline (`--repeat` + `sample`) before continuing** —
   the §2 numbers predate these landings.
2. §4.2 allocator quadratics + §4.3 predecessor sharing + §4.1 liveness
   fusion (bc-lower/ir-optimize structural cleanups).
3. §4.4-4.6 parser dispatch + declaration-in-place + assigned-vars memo.
4. Decide the remaining big bets: ~~§5.1 interning~~ (abandoned, §7.6) and
   ~~§5.2a instruction pool~~ (neutral, §7.6) are done; §5.3 flat slot
   environment is the live Tier-3 item, then §5.4 SoA tokens / §5.5 parse
   parallelism as appetite allows.

Re-profile (`sample` + `--repeat`) after each tier; the ranking above assumes
the baseline mix, and landing Tier 1 will shift it.

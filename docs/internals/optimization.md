# SSA IR Optimization

Optimization passes on Roxy's SSA IR. Phase 1 (constant folding, algebraic simplification, cast folding) runs eagerly during IR construction (`emit_binary` / `emit_unary` / `gen_primitive_cast`, helpers in `ir_fold.cpp`). Phases 2–4 live in `compiler/ir/ir_optimize.{hpp,cpp}` (tests: `tests/unit/test_ir_optimize.cpp`) and run from `Compiler::link_modules()` between coroutine lowering and IR validation:

```
1. Constant folding + algebraic simplification   (Phase 1, during IR building)
2. Copy propagation                              ┐
3. Dead code elimination                         │
4. Branch folding                                │ Phases 2–4, iterated
5. Block merging                                 │ to a fixed point
6. Trivial block-argument elimination            │
7. Local CSE                                      ┘
8. Reorder blocks (RPO) + metadata remap         (once, at the end)
```

Every pass strictly shrinks the IR, so the fixed-point loop is bounded. The final `IRFunction::reorder_blocks_rpo()` drops blocks made unreachable (e.g. a folded branch's dead arm) and remaps `BlockId`s in terminators and exception/finally/cleanup metadata.

## Shared Infrastructure

- **`IRFunction::inst_for(ValueId)`** returns the defining instruction, or `nullptr` for function/block params (treated as non-constant). Removed values are poisoned to `nullptr` so later passes catch stale lookups.
- **Operand enumeration** — `for_each_operand` / `for_each_terminator_operand` hand the callback a *mutable* `ValueId&`, so one helper serves reading and rewriting. Gotcha: `SetField` has **two** operands — `field.object` and the top-level `store_value`.
- **Side effects** — `has_side_effect(IROp)` in `ir_optimize.cpp` is the authoritative list (writes, counting, lifecycle, calls, pins, traps, `Throw`/`Yield`). **`Nullify` is on it and is load-bearing**: lowering reads its position to narrow cleanup scope after a `uniq` move; removing it would re-destroy moved-from owned locals.
- **Substitution** — copy-prop, trivial-arg-elim, and CSE share a function-wide path-compressed union-find `subst[]`; redirected values fall to zero uses and the next DCE run drops them.

## Phase 1: IR Builder Optimizations

Applied during IR construction by inspecting operands before emitting — no separate pass; only `Const*` operands count as constant.

### Constant Folding

Arithmetic, comparisons, logical, bitwise, and conversions on constant operands (`ir_fold.cpp`). Deliberate exceptions:

- `And`/`Or` are folded even though short-circuit `&&`/`||` lower to branches — they appear in case-condition merging (`gen_when_stmt`).
- **Float folding** uses host IEEE-754 arithmetic for add/sub/mul/div/neg. Float *comparisons* are not folded (NaN ordering), and `-(-x)` is not simplified (`-(-0.0)` differs in sign bit, which matters for `1.0/x`).
- **Division by zero** and `INT64_MIN / -1` are not folded — the original `DivI`/`ModI` is preserved so the runtime produces "Division by zero". A compile-time error would need source locations threaded through `emit_binary`.

### Algebraic Simplifications

Identity, absorbing, self-cancelling and double-negation forms, matched in `emit_binary` / `emit_unary` (`ir_builder_expr.cpp`). `gen_primitive_cast` folds constant casts (`fold_cast_const`), mirroring the runtime cast semantics.

Strength reduction is limited to `mul x, 2 → add x, x`. `mul x, pow2 → shl` is skipped (no register-VM win); `div x, pow2 → shr` is skipped because Roxy's `i32`/`i64` are signed and arithmetic shift-right doesn't match signed division for negatives (`(-1)/2 == 0` but `(-1)>>1 == -1`).

## Phase 2: Use-Count Based Passes

Both passes build on per-`ValueId` use counts. **DCE** is worklist-based over zero-use, side-effect-free instructions (`BlockArg` excluded — block-param trimming is Phase 3).

### Copy Propagation

Each `IROp::Copy` is redirected to its source via `subst`, rewriting all operands *except* the Copy's own (so DCE's operand debit still points at the true source); the dead Copies are then removed by DCE.

**Pure-Copy assumption:** a propagatable `IROp::Copy` is a borrow that retypes a uniq pointer as a ref pointer (same representation, 1 slot), always semantically `result := source`. A future Copy with runtime meaning (e.g. a strong-ref bump) must use a new opcode and be added to `has_side_effect`.

**The `no_copy_prop` exception:** some `Copy`s carry an identity that later cleanup names, and `is_propagatable_copy` skips those (`IRInst::no_copy_prop`). Two kinds:

- **Call-site heap-root borrows** — the flag keeps the borrow a *distinct SSA value*, hence a distinct register from the receiver it straddles, so its `RefDec` + `Nullify` cleanup cannot clobber the owner's own `Delete` record. See [lifetimes.md](lifetimes.md) → "Call-site heap-root borrows".
- **`ref` locals** (`var r: ref T = ref x`) — the local's cleanup record and its scope-exit `Nullify` both name the Copy's ValueId. Folding it into the source retargets them at the source: `var current: ref Node = ref node` emitted `nullify node; ref_dec node`, releasing a nulled pointer and leaving the param's borrow uncounted. Pinned in `pin_tracked_value` where the local is tracked.

The general rule: **a `Copy` whose ValueId is recorded anywhere outside the IR — a cleanup record, a drop plan, a `Nullify` — is not a pure copy, and propagating it silently retargets that record.**

## Phase 3: Control Flow Optimizations

Predecessors are recomputed per pass rather than cached on `IRBlock` (a cache would go stale across passes).

### Branch Folding

A `Branch` on a `ConstBool` becomes a `Goto` of the taken target. Only `ConstBool` is recognized (Phase 1 doesn't normalize integer-truthy conditions); it never needs to look through a `Copy` because copy-prop ran first.

### Block Merging

A block B whose sole predecessor A ends in `Goto B(args)` is spliced into A (params substituted by the args), removing the jump and the block-argument MOVs lowering would emit.

**Metadata safety:** `block_in_metadata` skips the merge if B's `BlockId` appears in any `IRExceptionHandler`, `IRFinallyInfo`, or `IRCleanupInfo` (try/finally/cleanup ranges are order-sensitive and delicate to rewrite). Most merges are in non-exception, non-RAII code and proceed.

### Trivial Block Argument Elimination

A non-entry block param is replaced by the value all predecessors pass, ignoring self-references (a back-edge passing the param itself). Real loop params (entry and back-edge disagree) are preserved.

## Phase 4: Local Value Numbering

Block-local Common Subexpression Elimination: within one block, identical pure operations reuse the first result.

- **Eligibility** (`is_cse_eligible`): pure value ops (constants, arithmetic, comparisons, logical, bitwise, conversions, `Cast`). **Excluded** for safety: memory loads (`GetField`, `GetFieldAddr`, `LoadPtr`, `IndexGet` — may alias intervening writes), `WeakCheck`/`WeakCreate` (slab generation state), `StackAlloc` (fresh address), `BlockArg`, `Copy`, and anything with a side effect.
- **Key** (`CSEKey`): float constants key on their bit pattern (keeps `+0.0`/`-0.0` distinct); `Cast` includes the source type; `ConstString` keys on `(data, size)` — interned lexer buffers share identity, so equal literals collapse. `Type*` identity is reliable because types are interned in `TypeEnv`.
- **Scope**: the table is cleared between blocks — cross-block CSE needs dominator info (global CSE/GVN, deferred). Commutativity is not exploited (`AddI a b` ≠ `AddI b a`).
- **Placement**: inside the fixed-point loop before the Phase 2 re-run, so DCE drops the duplicates and block merging keeps feeding it longer straight-line blocks.

## Future Phases

Deferred — these need more infrastructure:

- **Global CSE / GVN** — dominator tree.
- **Loop-Invariant Code Motion** — loop detection and dominance frontiers.
- **Function Inlining** — call-graph analysis; careful scope/RAII/handler handling.
- **Tail Call Optimization** — tail-call detection and specialized bytecode.
- **Escape Analysis** — interprocedural analysis for stack-allocating heap objects.

# SSA IR Optimization

Optimization passes on Roxy's SSA IR. Phase 1 (constant folding, algebraic simplification, cast folding) runs eagerly during IR construction; Phases 2–4 (DCE + copy propagation, control-flow simplification, block-local CSE) run as standalone passes between IR building and lowering.

**Current state:** Phase 1 is implemented in `IRBuilder::emit_binary` / `emit_unary` / `gen_primitive_cast`. Phases 2–4 live in `compiler/ir_optimize.{hpp,cpp}` and run from `Compiler::link_modules()` between coroutine lowering and IR validation:

- **Phase 2** — use-count computation, dead code elimination, copy propagation.
- **Phase 3** — branch folding, block merging, trivial block-argument elimination.
- **Phase 4** — block-local Common Subexpression Elimination.

The driver iterates `(branch fold → block merge → trivial-arg-elim → local CSE → re-run Phase 2)` to a fixed point, then finishes with `IRFunction::reorder_blocks_rpo()` to drop newly-unreachable blocks and remap `BlockId` references in exception/finally/cleanup metadata.

```
Source → … → IR Builder → SSA IR → [Optimization] → Lowering → Bytecode → VM
```

## Shared Infrastructure

- **`values_by_id`** — `IRFunction::values_by_id` is a `Vector<IRInst*>` indexed by `ValueId.id`; `IRFunction::inst_for(ValueId)` returns the defining instruction or `nullptr` (function/block params, which are treated as non-constant). Removed values are poisoned to `nullptr` so later passes catch stale lookups.
- **Operand enumeration** — `for_each_operand(IRInst*, Fn)` / `for_each_terminator_operand(Terminator&, Fn)` are inline templates switching over every `IROp`. The callback receives a *mutable* `ValueId&`, so one helper serves both reading (use-count, DCE) and rewriting (copy-prop, CSE, arg-elim). Gotcha: `SetField` has **two** operands — `field.object` and the top-level `store_value`.
- **Side-effect classification** — `has_side_effect(IROp)` is true for memory writes (`SetField`, `StorePtr`, `StructCopy`, `IndexSet`), ref counting (`RefInc`/`RefDec`), object lifecycle (`New`/`Delete` — user ctors/dtors have arbitrary effects), calls (`Call`/`CallNative`/`CallExternal`), control-flow/coroutine (`Throw`/`Yield`), and **`Nullify`**. `Nullify` is load-bearing: lowering reads its position to narrow cleanup scope after a `uniq` move; removing it would re-destroy moved-from owned locals.
- **Substitution** — copy-prop, trivial-arg-elim, and CSE all build a function-wide `subst[id]` union-find table (path-compressed), then rewrite operands via `for_each_operand`. Redirected values fall to zero uses and are dropped by the next DCE run.

## Phase 1: IR Builder Optimizations

Applied during IR construction by inspecting operands before emitting — no separate pass or use-def analysis. `inst_for` provides the defining instruction; only `Const*` operands are treated as constant.

### Constant Folding

Evaluate operations on constant operands at compile time. Covers arithmetic, comparisons, logical (`Not`/`And`/`Or`), bitwise, and conversions for i32/i64/f32/f64.

```
v0 = const_i 3            v2 = const_i 8
v1 = const_i 5     →
v2 = add_i v0, v1
```

Notes:
- `And`/`Or` are folded even though short-circuit `&&`/`||` lower to branches — they appear in case-condition merging (`gen_when_stmt`).
- **Float folding** uses host IEEE-754 arithmetic for add/sub/mul/div/neg. Float *comparisons* are not folded (NaN ordering), and `-(-x)` is not simplified (`-(-0.0)` differs in sign bit, which matters for `1.0/x`).
- **Division by zero** and `INT64_MIN / -1` are not folded — the original `DivI`/`ModI` is preserved so the runtime produces "Division by zero". A compile-time error would need source locations threaded through `emit_binary`.

### Algebraic Simplifications

Pattern-match identity / absorbing / self-cancelling forms in `emit_binary`/`emit_unary`:

- Identity: `add x,0`, `sub x,0`, `mul x,1`, `div x,1`, `bit_and x,~0`, `bit_or x,0`, `bit_xor x,0`, `shl/shr x,0` → `x`
- Absorbing: `mul x,0` → `0`, `bit_and x,0` → `0`, `bit_or x,~0` → `~0`
- Self-cancelling: `sub x,x` → `0`, `bit_xor x,x` → `0`
- Double negation: `neg(neg x)` → `x`, `not(not x)` → `x`

### Cast Folding

`gen_primitive_cast` folds constant casts, mirroring the runtime's `emit_cast_bytecode` semantics (TRUNC_S/TRUNC_U for narrowing, sign-extend when widening signed sources).

Strength reduction is limited to `mul x, 2 → add x, x`. `mul x, pow2 → shl` is skipped (no register-VM win); `div x, pow2 → shr` is skipped because Roxy's `i32`/`i64` are signed and arithmetic shift-right doesn't match signed division for negatives (`(-1)/2 == 0` but `(-1)>>1 == -1`).

## Phase 2: Use-Count Based Passes

`compute_use_counts` walks every instruction and terminator, counting references per `ValueId`. Both DCE and copy-prop build on this.

### Dead Code Elimination

Worklist-based. Seeds with every instruction whose result has zero uses, no side effect, and `op != BlockArg` (block-arg trimming is Phase 3). Removing an instruction decrements its operands' counts; operands reaching zero are re-queued. Each block's instruction vector is compacted with the two-finger write-index pattern, then truncated with `pop_back()` (`Vector::resize()` would reallocate and copy out-of-range slots when shrinking).

```
v0 = const_i 3   (unused, no side effect)   →   (removed)
```

### Copy Propagation

Union-find with path compression: for each `IROp::Copy`, set `subst[copy.result] = copy.unary`, path-compress, then rewrite all non-Copy operands and terminators. Skipping the Copy's own operand keeps DCE's operand debit pointing at the true source; the now-dead Copies fall to zero uses and DCE removes them.

```
v5 = copy v3              ... uses v3 ...
... uses v5 ...     →     (copy removed by DCE)
```

**Pure-Copy assumption:** every `IROp::Copy` is emitted by `gen_unary_expr` for `ref expr` — a borrow that retypes a uniq pointer as a ref pointer (same representation, 1 slot), always semantically `result := source`. A future Copy with runtime meaning (e.g. a strong-ref bump) must use a new opcode and be added to `has_side_effect`.

Phase 2 only removes/rewrites in place — it never creates unreachable blocks, so no extra `reorder_blocks_rpo()` is needed for it alone.

## Phase 3: Control Flow Optimizations

Predecessors are recomputed locally via `compute_predecessors(IRFunction*)` rather than caching `IRBlock::predecessors` (which would go stale across passes).

### Branch Folding

`run_branch_folding` scans every `Branch`; if the condition's defining instruction is `IROp::ConstBool`, it rewrites the terminator to a `Goto` of the taken target (preserving args). Conditions that are constant but not `ConstBool` (e.g. `ConstInt`) are not folded — Phase 1 doesn't normalize integer-truthy conditions. Copy chains are already collapsed by the preceding copy-prop, so it never looks through a Copy.

```
b0:                                  b0:
    v0 = const_bool true        →        goto b1(args...)
    if v0 goto b1(..) else b2(..)
```

The dead arm (`b2`) becomes unreachable and is dropped by `reorder_blocks_rpo()`.

### Block Merging

`run_block_merging` finds a block B whose sole predecessor A ends with an unconditional `Goto B(args)`. It substitutes `B.params[i] → A.goto.args[i]` (union-find), appends B's instructions to A, replaces A's terminator with B's, and empties B in place for RPO cleanup. Self-loops and arg-count mismatches are skipped. This removes the jump and the block-argument MOVs that lowering would emit.

```
b0:                          b0:
    v0 = add_i v1, v2            v0 = add_i v1, v2
    goto b1(v0)          →       v4 = mul_i v0, v0
b1(v3):                          return v4
    v4 = mul_i v3, v3
    return v4
```

**Metadata safety:** `block_in_metadata` skips the merge if B's `BlockId` appears in any `IRExceptionHandler`, `IRFinallyInfo`, or `IRCleanupInfo` (try/finally/cleanup ranges are order-sensitive and delicate to rewrite). Most merges are in non-exception, non-RAII code and proceed.

### Trivial Block Argument Elimination

`run_trivial_block_arg_elim`: for each non-entry block param, `arg_for_target` fetches the value each predecessor passes. Self-references (a loop back-edge passing the param itself) are stripped from the unanimity check. If the remaining preds all agree on one value, the param is replaced function-wide (union-find), dropped from B's params, and removed from each predecessor's jump target. Loop params (entry passes 0, back-edge passes the updated value) genuinely disagree and are preserved.

```
b1: goto b3(v0)              b1: goto b3()
b2: goto b3(v0)        →     b2: goto b3()
b3(v5): ... uses v5 ...      b3(): ... uses v0 ...
```

Dropped args compact the `Span<BlockArgPair>` in place (trailing bump-allocator slots leak harmlessly — compile lifetime is bounded).

### Driver fixed point

Phase 3 passes run in a `while (changed)` loop that re-runs Phase 2 (copy-prop + DCE) each iteration to clean up values exposed by CFG mutation (folded `ConstBool` conditions, orphaned arguments). Every pass strictly shrinks the IR, so the loop is bounded. A single `reorder_blocks_rpo()` at the end removes blocks unreachable from entry and remaps every `BlockId` in terminators and exception/finally/cleanup metadata.

## Phase 4: Local Value Numbering

Block-local Common Subexpression Elimination: within one block, identical pure operations reuse the first result.

```
b0:                              b0:
    v0 = add_i v1, v2                v0 = add_i v1, v2
    v3 = mul_i v0, v4        →       v3 = mul_i v0, v4
    v5 = add_i v1, v2                v6 = mul_i v0, v7   // reuses v0
    v6 = mul_i v5, v7
```

- **Eligibility** (`is_cse_eligible`): all `Const*`, arithmetic, comparisons, logical (`Not`/`And`/`Or`), bitwise, conversions, and `Cast`. **Excluded** for safety: memory loads (`GetField`, `GetFieldAddr`, `LoadPtr`, `IndexGet` — may alias intervening writes), weak-ref reads (`WeakCheck`/`WeakCreate` — slab generation state), fresh-address ops (`StackAlloc`), `BlockArg`, `Copy`, and everything `has_side_effect` covers.
- **Key** (`CSEKey`): `(op, result_type, a, b, payload)`. Binary uses `(left, right, 0)`; unary `(operand, 0, 0)`; `Cast` carries the source type to disambiguate conversion strategy; constants encode their literal in `payload` (bit-patterns for floats keep `+0.0`/`-0.0` distinct); `ConstString` uses `(data, size, 0)` — interned lexer buffers share identity, so equal literals collapse. `Type*` identity is reliable (types are interned in `TypeEnv`). Hashing uses the FNV-1a prime as a multiplier.
- **Algorithm**: per block, a `tsl::robin_map<CSEKey, ValueId>` records first-seen wins; equivalents go into the shared `subst` table. The map is cleared between blocks — cross-block CSE needs dominator info (global CSE/GVN, deferred). Commutativity is not exploited in v1 (`AddI a b` ≠ `AddI b a`).
- **Placement**: inside the Phase 3 fixed-point loop, after trivial-arg-elim and before the Phase 2 re-run, so DCE drops the dead duplicates and block merging keeps feeding it longer straight-line blocks.

## Pass Ordering

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

## Future Phases

Deferred — these need more infrastructure:

- **Global CSE / GVN** — dominator tree.
- **Loop-Invariant Code Motion** — loop detection and dominance frontiers.
- **Function Inlining** — call-graph analysis; careful scope/RAII/handler handling.
- **Tail Call Optimization** — tail-call detection and specialized bytecode.
- **Escape Analysis** — interprocedural analysis for stack-allocating heap objects.

## Files

| File | Purpose |
|---|---|
| `include/roxy/compiler/ir_builder.hpp` / `src/roxy/compiler/ir_builder.cpp` | Phase 1 (fold / simplify / cast fold during IR building) |
| `include/roxy/compiler/ir_optimize.hpp` / `src/roxy/compiler/ir_optimize.cpp` | Phases 2–4 passes and fixed-point driver |
| `include/roxy/compiler/ssa_ir.hpp` / `src/roxy/compiler/ssa_ir.cpp` | IR data structures, `reorder_blocks_rpo`, printing |
| `include/roxy/compiler/lowering.hpp` / `src/roxy/compiler/lowering.cpp` | IR → bytecode lowering |
| `tests/unit/test_ir_optimize.cpp` | Phase 2–4 unit tests |

# SSA IR Optimization

Design plan for optimization passes on Roxy's SSA IR. Passes are organized into phases by implementation complexity and infrastructure requirements.

**Current state:** Phase 1 (constant folding, algebraic simplifications, cast folding) is implemented in `IRBuilder::emit_binary` / `emit_unary` / `gen_primitive_cast`. Phases 2–4 are implemented as standalone passes in `compiler/ir_optimize.{hpp,cpp}`, run from `Compiler::link_modules()` between coroutine lowering and IR validation: Phase 2 (use-count computation, dead code elimination, copy propagation), Phase 3 (branch folding, block merging, trivial block-argument elimination), and Phase 4 (block-local Common Subexpression Elimination). The driver iterates `(branch fold → block merge → trivial-arg-elim → local CSE → re-run Phase 2)` to a fixed point and finishes with `IRFunction::reorder_blocks_rpo()` to drop newly-unreachable blocks and remap BlockId references in exception/finally/cleanup metadata.

## Overview

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → [Optimization] → Lowering → Bytecode → VM
```

Optimizations slot in between IR building and lowering. Some optimizations (constant folding, algebraic simplifications) can also be applied eagerly during IR building for zero overhead.

## Phase 1: IR Builder Optimizations (No New Infrastructure) — IMPLEMENTED

These optimizations are applied during IR construction by checking operands before emitting instructions. No separate pass or use-def analysis needed.

### Implementation notes

- **Infrastructure:** `IRFunction::values_by_id` is a `Vector<IRInst*>` indexed by `ValueId.id`, populated by `IRFunction::new_value()` (nullptr) and `IRBuilder::emit_inst()` (real pointer). Lookup via `IRFunction::inst_for(ValueId)` returns the defining instruction or nullptr (for function/block params, which fold treats as non-constant). This table is reused by later phases (DCE, CSE).
- **`And` / `Or` IR ops** are folded too. They aren't emitted by short-circuit `&&`/`||` (those lower to branches), but they are emitted in case-condition merging (`gen_when_stmt`).
- **Cast folding** is added to `gen_primitive_cast` in addition to fold/simplify on `emit_binary` / `emit_unary`. Folded casts mirror the runtime's `emit_cast_bytecode` semantics (TRUNC_S/TRUNC_U for narrowing, sign-extend on widening signed sources).
- **Division by zero / `INT64_MIN / -1`:** not folded — the original `DivI` / `ModI` instruction is preserved so the runtime produces "Division by zero" (`src/roxy/vm/interpreter.cpp:585,595`). Compile-time error would require source-location threading through `emit_binary`.
- **Float folding:** add/sub/mul/div/neg are folded for f32 and f64 using host arithmetic, assuming an IEEE-754 host (true on all currently-targeted platforms). Float comparisons are NOT folded in this phase to avoid NaN-ordering subtleties. Float double-negation (`-(-x)`) is NOT simplified because `-(-0.0)` differs from `-0.0` in sign bit, which matters for `1.0/x`.
- **Strength reduction:** only `mul x, 2 → add x, x` is implemented. `mul x, pow2 → shl` is skipped (the register VM has no measurable advantage for SHL over MUL). `div x, pow2 → shr` is skipped because Roxy's `i32` / `i64` are signed and arithmetic shift right does not match signed division for negative values (e.g., `(-1) / 2 == 0` but `(-1) >> 1 == -1`).

### Constant Folding

Evaluate operations on constant operands at compile time.

**Before:**
```
v0 = const_i 3
v1 = const_i 5
v2 = add_i v0, v1
```

**After:**
```
v2 = const_i 8
```

**Foldable operations:**
- Arithmetic: `add_i`, `sub_i`, `mul_i`, `div_i`, `mod_i`, `neg_i` (and f32/f64 variants)
- Comparisons: `eq_i`, `ne_i`, `lt_i`, `le_i`, `gt_i`, `ge_i` (and f32/f64 variants)
- Logical: `not`, `and`, `or`
- Bitwise: `bit_and`, `bit_or`, `bit_xor`, `bit_not`, `shl`, `shr`
- Conversions: `i_to_f64`, `f64_to_i`, `i_to_b`, `b_to_i`

**Implementation:** In `emit_binary()`/`emit_unary()`, check if all operands are `Const*` ops. If so, compute the result and emit a constant instead.

### Algebraic Simplifications

Pattern-match and simplify operations with known identity/absorbing elements.

**Identity rules:**
- `add_i x, 0` → `x`
- `sub_i x, 0` → `x`
- `mul_i x, 1` → `x`
- `div_i x, 1` → `x`
- `bit_and x, ~0` → `x`
- `bit_or x, 0` → `x`
- `bit_xor x, 0` → `x`
- `shl x, 0` → `x`, `shr x, 0` → `x`

**Absorbing rules:**
- `mul_i x, 0` → `const_i 0`
- `bit_and x, 0` → `const_i 0`
- `bit_or x, ~0` → `const_i ~0`

**Self-cancellation:**
- `sub_i x, x` → `const_i 0`
- `bit_xor x, x` → `const_i 0`

**Double negation:**
- `neg_i (neg_i x)` → `x`
- `not (not x)` → `x`

**Strength reduction:**
- `mul_i x, 2` → `add_i x, x`
- `mul_i x, power_of_2` → `shl x, log2(n)`
- `div_i x, power_of_2` → `shr x, log2(n)` (unsigned only)

**Implementation:** Same approach as constant folding — check patterns in `emit_binary()`/`emit_unary()` and emit simplified instructions.

## Phase 2: Use-Count Based Passes — IMPLEMENTED

These passes require computing how many times each `ValueId` is used. This is a lightweight analysis: walk all instructions and terminators, count references to each value.

### Implementation notes

- **Files:** `include/roxy/compiler/ir_optimize.hpp`, `src/roxy/compiler/ir_optimize.cpp`. Tests in `tests/unit/test_ir_optimize.cpp`.
- **Operand enumeration helper:** `for_each_operand(IRInst*, Fn)` and `for_each_terminator_operand(Terminator&, Fn)` are header-defined inline templates with a switch over every `IROp`. The callback receives a *mutable* `ValueId&`, so the same helper serves both reading (use-count, DCE) and rewriting (copy propagation). Block-argument operands inside `Goto`/`Branch` terminators are visited by `for_each_terminator_operand`. **`SetField` has TWO operands**: `inst->field.object` (in the field union) and `inst->store_value` (a top-level `IRInst` field — easy to miss).
- **Side-effect classification:** `has_side_effect(IROp)` returns true for memory writes (`SetField`, `StorePtr`, `StructCopy`, `IndexSet`), reference counting (`RefInc`, `RefDec`), object lifecycle (`New`, `Delete` — Roxy supports user-defined constructors/destructors with arbitrary side effects), calls (`Call`, `CallNative`, `CallExternal`), control-flow / coroutine (`Throw`, `Yield`), and **`Nullify`**. `Nullify` is critical: lowering reads its position via `m_nullify_pcs` to narrow cleanup scope after a `uniq` move; removing it would re-destroy moved-from owned locals.
- **DCE algorithm:** worklist-based. Seeds the worklist with every instruction whose result has zero uses, no side effect, and `op != BlockArg` (block-arg trimming is Phase 3). For each removed instruction, decrements the use counts of its operands; any operand whose count reaches zero is added to the worklist. Compacts each block's instruction vector via the two-finger write-index pattern, then truncates with `pop_back()` (Vector::resize() reallocates and would copy out-of-range slots when shrinking). Removed values get `values_by_id[id] = nullptr` (poisoning) so later passes catch stale lookups.
- **Copy propagation algorithm:** union-find with path compression. `subst[id] = id` initially; for each `IROp::Copy` instruction, set `subst[copy.result.id] = copy.unary.id`; path-compress (halving). Then walk all blocks and rewrite operands of every non-Copy instruction (and every terminator) via the substitution. Skipping rewrite of Copy's own operand keeps DCE's operand-decrement debit pointing at the correct source. After copy-prop runs, dead Copies have zero uses → DCE removes them.
- **Pure-Copy assumption:** every existing `IROp::Copy` is emitted by `gen_unary_expr` for `UnaryOp::Ref` (`ref expr` syntax) — a borrow that just retypes a uniq pointer as a ref pointer (same runtime representation, 1 slot). It is always semantically `result := source`. If a future change introduces a Copy with runtime meaning (e.g., a strong-ref bump), it must use a new opcode and be added to `has_side_effect()`.
- **Iteration:** one ordered pass (copy-prop → DCE) reaches a fixed point for Phase 2 alone. Copy-prop is monotone (single union-find sweep is exhaustive); DCE's worklist already iterates internally. When Phase 3 (branch folding, block merging) lands, the driver becomes a `while (changed)` loop. The `BumpAllocator&` parameter is plumbed now (unused by Phase 2) so Phase 3+ branch folding can allocate new constants without an API churn.
- **No CFG mutation:** Phase 2 only removes / rewrites in place; it never creates unreachable blocks, so a second `reorder_blocks_rpo()` call is not needed.

### Infrastructure: Use-Count Computation

```
fun compute_use_counts(func: IRFunction) -> Vector<u32>:
    counts = Vector<u32>(func.next_value_id, 0)
    for each block in func.blocks:
        for each inst in block.instructions:
            for each operand in inst:
                counts[operand.id] += 1
        for each operand in block.terminator:
            counts[operand.id] += 1
    return counts
```

Requires a helper to enumerate all `ValueId` operands of an instruction (similar to the existing switch in `lowering.cpp` for liveness marking).

### Dead Code Elimination

Remove instructions whose results are never used.

**Algorithm:**
1. Compute use counts for all values.
2. Build a worklist of instructions with zero uses.
3. For each dead instruction, decrement use counts of its operands. If any operand's count drops to zero, add it to the worklist.
4. Repeat until worklist is empty.
5. Remove all dead instructions from their blocks.

**Side-effectful instructions must be preserved** even with zero uses:
- Memory writes: `SetField`, `StorePtr`, `StructCopy`
- Reference counting: `RefInc`, `RefDec`
- Object lifecycle: `New`, `Delete`
- Calls: `Call`, `CallNative`, `CallExternal` (may have side effects)
- Control flow: `Throw`, `Yield`

### Copy Propagation

Replace all uses of `v5 = copy v3` with `v3`, then remove the dead copy.

**Algorithm:**
1. Find all `Copy` instructions.
2. For each copy `vN = copy vM`, replace all occurrences of `vN` in subsequent instructions/terminators with `vM`.
3. Run dead code elimination to clean up.

**Implementation:** Requires a value replacement helper that walks all instructions and rewrites operands via a `ValueId → ValueId` substitution map.

## Phase 3: Control Flow Optimizations — IMPLEMENTED

These passes simplify the control flow graph. They build on Phase 2's infrastructure.

### Implementation notes

- **Files:** `include/roxy/compiler/ir_optimize.hpp`, `src/roxy/compiler/ir_optimize.cpp`. Tests in `tests/unit/test_ir_optimize.cpp`.
- **Predecessor computation:** `compute_predecessors(IRFunction*)` returns a `Vector<Vector<BlockId>>` indexed by block.id. We don't populate `IRBlock::predecessors` because that field is unused outside `reorder_blocks_rpo` and could go stale across passes. Each Phase 3 pass that needs predecessors recomputes them locally; block merging refreshes after each successful merge.
- **Branch folding** (`run_branch_folding`): scans every `Branch` terminator. If the condition's defining instruction is `IROp::ConstBool`, rewrites the terminator to a `Goto` of the taken target's `JumpTarget` (preserving its args). Conditions that are constant-but-not-`ConstBool` (e.g. `ConstInt`) are NOT folded — Phase 1 doesn't normalize integer-truthy conditions. Copy chains are already collapsed by the Phase 2 copy-prop run that precedes Phase 3, so we never look through a Copy to find a constant.
- **Block merging** (`run_block_merging`): finds block B whose sole predecessor A ends with an unconditional `Goto B(args)`. Substitutes `B.params[i] -> A.goto_target.args[i]` (using the same union-find substitution pattern as copy-prop), rewrites operands of B's instructions and terminator, appends B's instructions to A, and replaces A's terminator with B's. B is emptied in place (params/instructions cleared, terminator set to `None`); `reorder_blocks_rpo()` at the end of `optimize_function` drops it. Self-loops (A == B) and arg-count mismatches are skipped. **Conservative metadata safety:** `block_in_metadata(func, B)` returns true if B's `BlockId` appears in any `IRExceptionHandler` (try_entry, try_exit, handler_block, try_body_blocks), `IRFinallyInfo` (try_entry, try_exit, finally_block, finally_end_block), or `IRCleanupInfo` (start_block, end_block); we skip the merge in that case. Rewriting metadata across a merge is delicate (try_body_blocks is order-sensitive, try_exit is the inclusive end of a contiguous bytecode range), so v1 errs on the safe side. Most merges happen in non-exception, non-RAII code and are unaffected.
- **Trivial block-argument elimination** (`run_trivial_block_arg_elim`): for each non-entry block B and each parameter `B.params[pi]`, walks every predecessor and uses `arg_for_target(P, B.id, pi)` to fetch the value passed for that parameter. Self-references (a predecessor passing the param itself, e.g. a loop back-edge) are stripped from the unanimity check. If all remaining predecessors agree on a single `common` value, the parameter is replaced (function-wide via union-find substitution), dropped from B's `params`, and the corresponding argument is dropped from each predecessor's jump target. Loop params (whose preds genuinely disagree — entry passes 0, back-edge passes the updated value) are preserved.
- **Span shrinking:** when dropping arguments from a `Span<BlockArgPair>`, we compact in place and reconstruct the Span with a smaller size (`jt.args = Span<BlockArgPair>(jt.args.data(), w)`). Trailing slots leak in the bump allocator, which is fine — compile lifetime is bounded.
- **Branch with both arms targeting the same block:** `arg_for_target` requires both arms to agree on the value for the parameter; if they disagree, the parameter is not trivial and is preserved.
- **Driver fixed point:** Phase 3 passes are wrapped in a `while (changed)` loop that re-runs Phase 2 (copy-prop + DCE) inside the loop to clean up dead values exposed by the CFG mutations (a folded ConstBool condition becomes dead, eliminated params orphan their old argument expressions, etc.). Each pass strictly reduces the IR (fewer branches/blocks/params), so the loop is bounded.
- **Final cleanup:** `func->reorder_blocks_rpo()` runs once at the end of `optimize_function`. It removes blocks unreachable from entry (folded-away arms, emptied merged-into blocks) and remaps every `BlockId` reference in terminators and in exception/finally/cleanup metadata.

### Branch Folding

If a `Branch` terminator's condition is a known constant, replace it with a `Goto` to the taken target.

**Before:**
```
b0:
    v0 = const_bool true
    if v0 goto b1(args...) else b2(args...)
```

**After:**
```
b0:
    goto b1(args...)
```

The dead branch target (b2) becomes unreachable and is removed by `reorder_blocks_rpo()`.

### Block Merging

If block B has exactly one predecessor A, and A's terminator is an unconditional `Goto` to B with no other successors, merge B into A.

**Before:**
```
b0:
    v0 = add_i v1, v2
    goto b1(v0)

b1(v3):
    v4 = mul_i v3, v3
    return v4
```

**After:**
```
b0:
    v0 = add_i v1, v2
    v4 = mul_i v0, v0
    return v4
```

This eliminates unnecessary jumps and the MOV instructions generated for block arguments during lowering.

**Algorithm:**
1. Compute predecessor counts for each block.
2. For each block B with exactly one predecessor A where A ends with `Goto` to B:
   - Substitute B's block parameters with the values A passes as arguments.
   - Append B's instructions to A.
   - Replace A's terminator with B's terminator.
   - Remove B.
3. Repeat until no more merges possible.

### Trivial Block Argument Elimination

If all predecessors of a block pass the same value for a block argument, replace the argument with that value.

**Before:**
```
b1:
    goto b3(v0)
b2:
    goto b3(v0)
b3(v5):
    ... uses v5 ...
```

**After:**
```
b1:
    goto b3()
b2:
    goto b3()
b3():
    ... uses v0 ...
```

## Phase 4: Local Value Numbering — IMPLEMENTED

### Implementation notes

- **Files:** `include/roxy/compiler/ir_optimize.hpp`, `src/roxy/compiler/ir_optimize.cpp`. Tests in `tests/unit/test_ir_optimize.cpp`.
- **Eligibility** (`is_cse_eligible`): all `Const*`, all arithmetic (i32/i64/f32/f64), all comparisons, logical (`Not`/`And`/`Or`), bitwise (`BitAnd`/`BitOr`/`BitXor`/`BitNot`/`Shl`/`Shr`), conversions (`I_TO_F64`, `F64_TO_I`, `I_TO_B`, `B_TO_I`), and `Cast`. **Excluded** for safety: memory loads (`GetField`, `GetFieldAddr`, `LoadPtr`, `IndexGet`) — could alias intervening writes; weak-ref reads (`WeakCheck`, `WeakCreate`) — slab generation state changes; fresh-address ops (`StackAlloc`, `VarAddr`); `BlockArg` (not emitted as a real instruction); `Copy` (already removed by Phase 2 copy-prop); plus everything `has_side_effect` covers.
- **Hash key** (`CSEKey`): `(IROp op, Type* result_type, u32 a, u32 b, u64 payload)`. Binary ops use `(left.id, right.id, 0)`; unary ops use `(operand.id, 0, 0)`; `Cast` carries `(source.id, 0, source_type ptr)` to disambiguate conversion strategy; constants encode their literal in `payload` (signed bits for `ConstInt`, bit-pattern for `ConstF`/`ConstD` so `+0.0` and `-0.0` keep distinct keys, `bool` as `0`/`1`); `ConstString` uses `(data_ptr, size, 0)` — interned string buffers from the lexer share identity, so identical literals collapse. `result_type` is needed only for `Cast` (different target types of the same source) but kept everywhere as a free safety check; `Type*` identity is reliable because types are interned in `TypeEnv`.
- **Hash function** uses the FNV-1a prime `1099511628211ULL` as a multiplier rather than `* 31 + x` chains, which collide on small inputs.
- **Algorithm**: for each block, build a `tsl::robin_map<CSEKey, ValueId, CSEKeyHash, CSEKeyEq>`. First-seen wins. Later equivalents are recorded in a function-wide `subst[id]` table (same shape as Phase 2 copy-prop and Phase 3 trivial-arg-elim). After scanning all blocks, path-compress the substitutions and rewrite all operands and terminator operands via `for_each_operand` / `for_each_terminator_operand`. The redirected duplicates have zero uses and are dropped by the next DCE run inside the driver loop.
- **Block-local scope**: the hash map is cleared between blocks. Cross-block redundancy elimination needs dominator information (global CSE / GVN, deferred). Phase 3 block merging produces longer straight-line blocks where Phase 4 sees more opportunities — running CSE inside the same fixed-point loop catches them.
- **Commutativity**: not exploited in v1 — `AddI a b` and `AddI b a` get distinct keys. Adding canonicalization (sort operands by `ValueId.id` for commutative ops) is a one-line change for later.
- **Driver placement**: inside the Phase 3 fixed-point loop, after `run_trivial_block_arg_elim` and before the re-run of Phase 2. Phase 2's DCE then drops the dead duplicates, possibly freeing operands that expose further optimization opportunities on the next iteration.

### Common Subexpression Elimination (Local)

Within a single block, if two instructions compute the same pure operation with the same operands, reuse the first result.

**Before:**
```
b0:
    v0 = add_i v1, v2
    v3 = mul_i v0, v4
    v5 = add_i v1, v2    // redundant
    v6 = mul_i v5, v7
```

**After:**
```
b0:
    v0 = add_i v1, v2
    v3 = mul_i v0, v4
    v6 = mul_i v0, v7    // reuses v0
```

**Algorithm:**
1. Maintain a hash map: `(IROp, operand1, operand2) → ValueId`.
2. Before emitting an instruction, check if the same expression already exists.
3. If found, replace the instruction with a reference to the existing value.
4. Only applies to pure operations (no calls, memory ops, side effects).

**Pure operations:** All arithmetic, comparison, logical, bitwise, and conversion ops.

## Future Phases (Not Low-Hanging Fruit)

These optimizations require more substantial infrastructure and should be deferred:

- **Global CSE / Global Value Numbering:** Requires dominator tree computation.
- **Loop-Invariant Code Motion:** Requires loop detection and dominance frontiers.
- **Function Inlining:** Requires call graph analysis, careful handling of scopes/RAII/exception handlers.
- **Tail Call Optimization:** Requires detecting tail-call patterns and emitting specialized bytecode.
- **Escape Analysis:** Requires interprocedural analysis for stack-allocating heap objects.

## Pass Ordering

Recommended pass ordering within a single optimization run:

```
1. Constant Folding + Algebraic Simplifications  (during IR building)
2. Copy Propagation
3. Dead Code Elimination
4. Branch Folding
5. Block Merging
6. Trivial Block Argument Elimination
7. Reorder Blocks (RPO) — already exists
8. Local CSE
9. Dead Code Elimination  (second round to clean up CSE)
```

Passes 2–9 can be iterated to a fixed point, but in practice one or two iterations should suffice.

## Files

- `include/roxy/compiler/ssa_ir.hpp` — IR data structures
- `src/roxy/compiler/ssa_ir.cpp` — IR utilities and printing
- `include/roxy/compiler/ir_builder.hpp` — IR building (Phase 1 optimizations go here)
- `src/roxy/compiler/ir_builder.cpp` — IR builder implementation
- `include/roxy/compiler/ir_optimize.hpp` — Phase 2 passes (DCE, copy propagation, use-count infra)
- `src/roxy/compiler/ir_optimize.cpp` — Phase 2 implementation
- `include/roxy/compiler/lowering.hpp` — IR to bytecode lowering
- `src/roxy/compiler/lowering.cpp` — Lowering implementation

# SSA IR Optimization

Design plan for optimization passes on Roxy's SSA IR. Passes are organized into phases by implementation complexity and infrastructure requirements.

**Current state:** Phase 1 (constant folding, algebraic simplifications, cast folding) is implemented in `IRBuilder::emit_binary` / `emit_unary` / `gen_primitive_cast`. Phases 2–4 are not yet implemented. The only structural cleanup is `reorder_blocks_rpo()`, which removes unreachable blocks.

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

## Phase 2: Use-Count Based Passes

These passes require computing how many times each `ValueId` is used. This is a lightweight analysis: walk all instructions and terminators, count references to each value.

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

## Phase 3: Control Flow Optimizations

These passes simplify the control flow graph. They build on Phase 2's infrastructure.

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

## Phase 4: Local Value Numbering

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
- `include/roxy/compiler/lowering.hpp` — IR to bytecode lowering
- `src/roxy/compiler/lowering.cpp` — Lowering implementation

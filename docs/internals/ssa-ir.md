# SSA IR

Roxy uses SSA (Static Single Assignment) IR with block arguments instead of phi nodes. The IR builder converts the AST into this form; lowering translates it to register-based bytecode (see `bytecode.md`).

## Block Arguments vs Phi Nodes

Traditional SSA uses phi nodes at block entry:
```
loop:
    sum = phi [0, entry], [sum2, body]
    i = phi [1, entry], [i2, body]
```

Roxy uses block arguments instead — successor values are passed at the jump site, and blocks declare parameters:
```
entry:
    goto loop(0, 1)              // initial values

loop(sum, i):                    // block parameters
    t0 = i <= n
    if t0 goto body else exit(sum)

body:
    sum2 = sum + i
    i2 = i + 1
    goto loop(sum2, i2)          // pass values to successor

exit(result):
    return result
```

This gives a cleaner dataflow representation that lowers directly to bytecode: block arguments become MOVs at jump sites, with no phi resolution pass.

## IR Structure

An `IRModule` holds `IRFunction`s, plus the module's struct types and globals; each function has block parameters, a return type, and a list of `IRBlock`s. A block has parameters, a list of `IRInst*`, and a terminator. Each `IRInst` carries an op, a result `ValueId`, a `Type*`, and a union of op-specific operand data (`ConstData`, `CallData`, `FieldData`, `IndexData`, …). Definitions are in `ssa_ir.hpp`, which is the authoritative list — the summary below groups the 93 ops:

```cpp
enum class IROp : u8 {
    // Constants (6)
    ConstNull, ConstBool, ConstInt, ConstF, ConstD, ConstString,

    // Arithmetic — integer (8, incl. unsigned div/mod)
    AddI, SubI, MulI, DivI, ModI, DivU, ModU, NegI,

    // Arithmetic — f32 (5) / f64 (5)
    AddF, SubF, MulF, DivF, NegF,
    AddD, SubD, MulD, DivD, NegD,

    // Comparisons — integer (10: signed 6 + unsigned ordered 4; eq/ne are bit-identical)
    EqI, NeI, LtI, LeI, GtI, GeI, LtU, LeU, GtU, GeU,

    // Comparisons — f32 (6) / f64 (6)
    EqF, NeF, LtF, LeF, GtF, GeF,
    EqD, NeD, LtD, LeD, GtD, GeD,

    // Logical (3) / Bitwise (7)
    Not, And, Or,
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr, UShr,

    // Conversions (4)
    I_TO_F64, F64_TO_I, I_TO_B, B_TO_I,

    // Memory (5) — GlobalAddr is a module-global slot address
    StackAlloc, GlobalAddr, GetField, GetFieldAddr, SetField,

    // References and strings (6)
    RefInc, RefDec, StrRetain, StrRelease, WeakCheck, WeakCreate,

    // Object lifecycle (2)
    New, Delete,

    // Closures / coroutines (3) — FuncIndex materializes a dispatch index
    Closure, FuncIndex, AssertHeap,

    // Functions / calls (4)
    Call, CallNative, CallExternal, CallIndirect,

    // Container indexing (6) — IndexTryAddr returns null instead of trapping;
    // ContainerPin/Unpin block realloc while an element is borrowed
    IndexGet, IndexSet, IndexAddr, IndexTryAddr, ContainerPin, ContainerUnpin,

    // Meta (2) / Structs (1) / Pointers (2) / Casting (1) / Cleanup (1)
    BlockArg, Copy,
    StructCopy,
    LoadPtr, StorePtr,
    Cast,
    Nullify,

    // Exceptions (1) / Coroutines (1)
    Throw,
    Yield,
};
// Total: 93 IR operations
```

## Terminators

Each block ends with a `Terminator` of one of five kinds: `None` (not yet terminated), `Goto` (unconditional jump with block-argument pairs), `Branch` (condition value plus then/else targets, each with its own argument pairs), `Return` (return value), and `Unreachable` (statically impossible fall-through, e.g. the no-`else` arm of an exhaustive `when`). See `ssa_ir.hpp`.

## Lowering to Bytecode

Block arguments become MOV instructions at jump sites:

```
entry:
    LOAD_INT  R1, 0              // sum = 0
    LOAD_INT  R2, 1              // i = 1

loop:
    LE_I      R3, R2, R0         // t0 = i <= n
    JMP_IF_NOT R3, exit

body:
    ADD_I     R1, R1, R2         // sum = sum + i
    ADD_I     R2, R2, 1          // i = i + 1
    JMP       loop

exit:
    MOV       R0, R1             // result = sum
    RET
```

`BytecodeBuilder` (`lowering.hpp`) drives this. Key decisions:

- **Two-pass emission** — the first pass records block offsets, the second patches jump targets.
- **Constant pool** — values that don't fit in a 16-bit immediate are emitted from the constant pool.
- **Block arguments** — lowered to MOVs before each jump.

### Register Allocation

The register file uses 8-bit indices (0–254, with 0xFF as a sentinel), a hard cap of 255 registers per function. Allocation is liveness-based with free-list reuse:

- **Liveness** computes def/last-use intervals over a linear program-point numbering. Definition points, operand last-uses, and block-param extension to each predecessor's terminator (parallel-assignment safety) are computed in **one fused forward walk** — they are independent and order-tolerant, since `mark_use` is a max over a shared numbering. Loop back-edge extension stays a separate pass (it reads finalized ranges) and iterates to a fixed point for nested loops.
- **Free-list reuse** is a 256-bit mask (`m_free_mask`) of available registers; values whose def and last-use lie within one block reclaim freed registers. Cross-block values and block params always get fresh registers to preserve zero-initialization for partially-defined values (e.g. AND/OR short-circuit).
- **Expiry** returns registers of passed last-uses to the free list before each allocation point, walking only the active set.
- **Call windows** are contiguous blocks (`dst`, multi-register return slots, then the argument block) placed by `reserve_call_window` at the lowest register **above every live value**, so dead space from expired values and earlier call windows is reused. Before this compaction, call-dense functions consumed a fresh register per call and hit the 255-register cliff. Function parameters are pre-colored to R0, R1, ….

### Register Spilling

When pressure exceeds 255 registers — the bump pointer is at the limit and the free list is empty — `spill_furthest()` evicts the active value with the latest last-use to the local stack, freeing its register. On the first spill, two scratch registers are permanently reserved (by evicting the two furthest-last-use values) to handle all subsequent reloads/spills during emission: spilled destinations write through `scratch[0]`, spilled operands are reloaded via `RELOAD_REG`, and spilled results are written back via `SPILL_REG`. Functions that never trigger spilling reserve no scratch registers and emit no spill/reload instructions.

## Files

| File | Purpose |
|---|---|
| `include/roxy/compiler/ir/ssa_ir.hpp` | IR data structures |
| `src/roxy/compiler/ir/ssa_ir.cpp` | IR utilities and printing |
| `include/roxy/compiler/ir/ir_builder.hpp` | AST → IR conversion |
| `src/roxy/compiler/ir/ir_builder.cpp` | Builder core: functions, globals, module init/shutdown |
| `src/roxy/compiler/ir/ir_builder_expr.cpp` / `ir_builder_stmt.cpp` | Expression / statement generation |
| `src/roxy/compiler/ir/ir_builder_lifetime.cpp` | Cleanup, destroy, and move-state emission |
| `src/roxy/compiler/ir/ir_fold.cpp` | Phase 1 constant folding / algebraic simplification |
| `src/roxy/compiler/ir/ownership_tracker.cpp` | Owned-local bookkeeping consulted by the builder |
| `src/roxy/compiler/ir/ir_optimize.cpp` | Phase 2–4 optimization passes ([optimization.md](optimization.md)) |
| `src/roxy/compiler/ir/ir_validator.cpp` | Structural IR validation (debug builds) |
| `include/roxy/compiler/codegen/lowering.hpp` | IR → bytecode conversion |
| `src/roxy/compiler/codegen/lowering.cpp` | Lowering implementation |

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

An `IRModule` holds `IRFunction`s; each function has block parameters, a return type, and a list of `IRBlock`s. A block has parameters, a list of `IRInst*`, and a terminator. Each `IRInst` carries an op, a result `ValueId`, a `Type*`, and a union of op-specific operand data (`ConstData`, `BinaryData`, `CallData`, `FieldData`, etc.). Definitions are in `ssa_ir.hpp`.

```cpp
enum class IROp : u8 {
    // Constants (6)
    ConstNull, ConstBool, ConstInt, ConstF, ConstD, ConstString,

    // Arithmetic - integer (6)
    AddI, SubI, MulI, DivI, ModI, NegI,

    // Arithmetic - f32 (5)
    AddF, SubF, MulF, DivF, NegF,

    // Arithmetic - f64 (5)
    AddD, SubD, MulD, DivD, NegD,

    // Comparisons - integer (6)
    EqI, NeI, LtI, LeI, GtI, GeI,

    // Comparisons - f32 (6)
    EqF, NeF, LtF, LeF, GtF, GeF,

    // Comparisons - f64 (6)
    EqD, NeD, LtD, LeD, GtD, GeD,

    // Logical (3)
    Not, And, Or,

    // Bitwise (6)
    BitAnd, BitOr, BitXor, BitNot, Shl, Shr,

    // Conversions (4)
    I_TO_F64, F64_TO_I, I_TO_B, B_TO_I,

    // Memory (4)
    StackAlloc, GetField, GetFieldAddr, SetField,

    // Reference counting (4)
    RefInc, RefDec, WeakCheck, WeakCreate,

    // Object lifecycle (2)
    New, Delete,

    // Closures (2)
    Closure, AssertHeap,

    // Functions / calls (4)
    Call, CallNative, CallExternal, CallIndirect,

    // Container indexing (2)
    IndexGet, IndexSet,

    // Meta (2)
    BlockArg, Copy,

    // Structs (1)
    StructCopy,

    // Pointers (2)
    LoadPtr, StorePtr,

    // Casting (1)
    Cast,

    // Cleanup (1)
    Nullify,

    // Exceptions (1)
    Throw,

    // Coroutines (1)
    Yield,
};
// Total: 80 IR operations
```

## Terminators

Each block ends with a `Terminator` of one of four kinds: `None` (not yet terminated), `Goto` (unconditional jump with block-argument pairs), `Branch` (condition value plus then/else targets, each with its own argument pairs), and `Return` (return value). See `ssa_ir.hpp`.

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

- **Liveness** computes def/last-use intervals over a linear program-point numbering, with extra passes for block-param extension (parallel-assignment safety), loop back-edge extension (fixed-point for nested loops), and same-block classification.
- **Free-list reuse** lets values whose def and last-use lie within one block reclaim freed registers; cross-block values and block params always get fresh registers to preserve zero-initialization for partially-defined values (e.g. AND/OR short-circuit).
- **Expiry** returns registers of passed last-uses to the free list before each allocation point. Function parameters are pre-colored to R0, R1, …, and call results use bump allocation to keep argument blocks contiguous for the calling convention.

### Register Spilling

When pressure exceeds 255 registers — the bump pointer is at the limit and the free list is empty — `spill_furthest()` evicts the active value with the latest last-use to the local stack, freeing its register. On the first spill, two scratch registers are permanently reserved (by evicting the two furthest-last-use values) to handle all subsequent reloads/spills during emission: spilled destinations write through `scratch[0]`, spilled operands are reloaded via `RELOAD_REG`, and spilled results are written back via `SPILL_REG`. Functions that never trigger spilling reserve no scratch registers and emit no spill/reload instructions.

## Files

| File | Purpose |
|---|---|
| `include/roxy/compiler/ssa_ir.hpp` | IR data structures |
| `src/roxy/compiler/ssa_ir.cpp` | IR utilities and printing |
| `include/roxy/compiler/ir_builder.hpp` | AST → IR conversion |
| `src/roxy/compiler/ir_builder.cpp` | IR builder implementation |
| `include/roxy/compiler/lowering.hpp` | IR → bytecode conversion |
| `src/roxy/compiler/lowering.cpp` | Lowering implementation |

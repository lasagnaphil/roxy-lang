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

`include/roxy/compiler/ir/ssa_ir.hpp` is authoritative for the data structures and the `IROp` enum (grouped and commented there). In outline: an `IRModule` holds `IRFunction`s plus struct types and globals; a function has params and `IRBlock`s; a block has parameters, a list of `IRInst*`, and a `Terminator` (`Goto`/`Branch` carry block-argument pairs; `Unreachable` marks a statically impossible fall-through, e.g. the no-`else` arm of an exhaustive `when`). Each `IRInst` carries an op, a result `ValueId`, a `Type*`, and a union of op-specific operand data.

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

`BytecodeBuilder` (`lowering.hpp`) drives this; jump targets are patched after all blocks are emitted.

### Register Allocation

The register file uses 8-bit indices (0–254, with 0xFF as a sentinel), a hard cap of 255 registers per function. Allocation is liveness-based with free-list reuse:

- **Liveness** computes def/last-use intervals over a linear program-point numbering. Block params are extended to each predecessor's terminator (parallel-assignment safety); loop back-edge extension iterates to a fixed point for nested loops.
- **Free-list reuse**: only values whose def and last-use lie within one block reclaim freed registers. Cross-block values and block params always get **fresh** registers, because the IR may contain values defined on only one branch (e.g. `&&`/`||` short-circuit) and lowering assumes a fresh register reads as zero. (Caveat: `CALL` zeroes a callee's non-parameter registers only in debug builds; release relies on write-before-read.)
- **Call windows** are contiguous blocks (`dst`, multi-register return slots, then the argument block) placed by `reserve_call_window` at the lowest register **above every live value**, so dead space is reused and call-dense functions don't march toward the 255-register cliff. Function parameters are pre-colored to R0, R1, ….

### Register Spilling

When pressure exceeds 255 registers — the bump pointer is at the limit and the free list is empty — `spill_furthest()` evicts the active value with the latest last-use to the local stack, freeing its register. On the first spill, two scratch registers are permanently reserved (by evicting the two furthest-last-use values) to handle all subsequent reloads/spills during emission: spilled destinations write through `scratch[0]`, spilled operands are reloaded via `RELOAD_REG`, and spilled results are written back via `SPILL_REG`. Functions that never trigger spilling reserve no scratch registers and emit no spill/reload instructions.

## Key Files

AST → IR: `compiler/ir/ir_builder.hpp` and the `ir_builder{,_expr,_stmt,_lifetime}.cpp` split. Phase 1 folding: `ir_fold.cpp`; Phase 2–4 passes: `ir_optimize.cpp` ([optimization.md](optimization.md)); structural checks: `ir_validator.cpp` (debug builds only). IR → bytecode: `compiler/codegen/lowering.{hpp,cpp}`.

# Bytecode Format

Roxy uses a 32-bit fixed-width register-based bytecode format.
`include/roxy/vm/bytecode.hpp` is authoritative: the `Opcode` enum (grouped by
hex range, each opcode commented with its semantics and operand layout), the
encode/decode helpers, `is_two_word_instruction`, and the
`BCConstant`/`BCFunction`/`BCModule` structures. This page covers the
conventions needed to read it.

## Instruction Encoding

All instructions are 32-bit fixed-width with three formats:

```
Format ABC:  [opcode:8][dst:8][src1:8][src2:8]    — 3-operand (arithmetic, comparisons)
Format ABI:  [opcode:8][dst:8][imm16:16]          — immediate (constants, loads)
Format AOFF: [opcode:8][reg:8][offset:16]         — branch/field access (jumps)
```

A few opcodes are **two-word**: the second `u32` carries a wider operand (see
below and `is_two_word_instruction`).

Range notes the enum doesn't make obvious at a glance:

- There are **no `AND`/`OR` opcodes** — bools are normalized 0/1, and `&&`/`||`
  lower to short-circuit branches in the IR builder.
- The ranges are not strictly categorical: overflow opcodes were packed into
  free slots (e.g. f64 RK compares and `JMP_IF_EQ_D_RK`/`JMP_IF_NE_D_RK` live
  in 0xD5–0xDC next to `THROW`/`DELETE`; `CALL_INDIRECT`/`ASSERT_HEAP` at
  0xDD/0xDE).

## Calling Convention

```
Arguments:  R(dst + ret_reg_count), R(dst + ret_reg_count + 1), ...  (left to right)
Return:     R0 of the callee window → caller's dst
```

Each function call allocates a new register window from the shared register
file. The argument window starts at `dst + ret_reg_count` (not `dst + 1`), so a
multi-register return never overlaps an argument slot. (`CALL_NATIVE` is the
exception: its arguments always start at `dst + 1`.)

### Returning multi-register values

Three return opcodes, distinguished by *where the value is*:

| Opcode | Value location | Used for |
|---|---|---|
| `RET` | `regs[a]` | everything that fits one register |
| `RET_STRUCT_SMALL` | `*regs[a]` — a **pointer** to ≤4 slots | small structs (a struct value lives in memory) |
| `RET_WEAK` | `regs[a]`, `regs[a+1]` — **inline** | `weak T` = `{pointer, generation}` |

`RET_WEAK` exists because a `weak` is the one multi-register value that is *not*
a pointer to memory: `WEAK_CREATE` writes the pair straight into two registers.
Returning it through `RET_STRUCT_SMALL` would dereference the pointer half and
hand back the pointee's first slots as the pair. `BCFunction::ret_reg_count` is
2 for a `weak` return.

## RK (Register-or-Constant) Encoding

To avoid `LOAD_INT`/`LOAD_CONST` materialization when an arithmetic op or
comparison operates against a compile-time constant (e.g. `i + 1`,
`zx2 + zy2 > 4.0`), each RK-eligible opcode has a parallel `*_RK` variant:

```
Format: [op_RK:8][dst:8][src1:8][const_idx:8]
            // dst = src1 OP K[const_idx]
```

The encoding mirrors ABC, but `c` is an 8-bit index into the function's
constant pool. The interpreter's specialized loaders (`rk_const_i64`,
`rk_const_f32`, `rk_const_f64`) bypass `load_constant`'s type-switch, relying on
a lowering invariant: `*_I_RK` references only `Int` constants, `*_D_RK` only
`Float`, and `*_F_RK` only `Int` constants holding f32 bit patterns.

**Lowering** (`compute_const_use_modes` in `lowering.cpp`): constants whose
uses are *all* RK-eligible skip both register allocation and `LOAD_*` emission.
Commutative ops are canonicalized so the constant lands on the RHS.

The constant-pool index is 8 bits: functions exceeding **256 constants** fall
back to materialization (`LOAD_CONST` uses a 16-bit `imm16`). Opcode-variant RK
was chosen to keep full 8-bit (256-entry) register fields, at the cost of more
opcodes.

**Integer comparison RK** (`*_I_RK`): defined but not emitted.
`fuse_compare_branch()` turns `LT_I + JMP_IF_NOT` into a single `JMP_IF_GE_I`
(one dispatch); an integer RK compare would lose that fusion (two dispatches).
Emitting it only pays once fused `JMP_IF_*_I_RK` variants exist.

## Two-Word Instructions

### Field Access

```
GET_FIELD: [GET_FIELD dst obj slot_count][slot_offset:16]
SET_FIELD: [SET_FIELD obj val slot_count][slot_offset:16]
```

`slot_count` (1–4) determines how much is read/written: 1 or 2 slots move a 32-
or 64-bit value through one register; 3 or 4 slots (a small struct) span two
consecutive registers.

### Fused Compare-and-Branch

Integer and f64 comparisons fuse with a following `JMP_IF`/`JMP_IF_NOT`:

```
Non-RK: [op:8][_:8][src1:8][src2:8] + [offset:i32]
RK:     [op:8][_:8][src1:8][const_idx:8] + [offset:i32]
```

Fusion is a post-emission peephole (`fuse_compare_branch`) that negates the
predicate when the branch is `JMP_IF_NOT`. Fusion drops the compare's register
write, so compares whose result is read in another block are recorded in
`m_unfusable_cmp_pcs` and skipped. Fused today: signed integer, f64, and f64 RK;
f32 and integer-RK fused branches do not exist.

### Function Calls

```
CALL:          [CALL:8 dst:8 _:8 arg_count:8][func_idx:32]
CALL_NATIVE:   [CALL_NATIVE:8 dst:8 _:8 arg_count:8][func_idx:32]
CALL_INDIRECT: [CALL_INDIRECT:8 dst:8 closure_reg:8 arg_count:8][reserved:32]
```

The 32-bit `func_idx` lifts the 256-function ceiling an 8-bit operand would
impose; `CALL_INDIRECT`'s second word is reserved for a future inline cache.

## Register Spill/Reload

When register pressure exceeds the 255-register limit, lowering spills
long-lived values to the local stack with ABI instructions
`SPILL_REG`/`RELOAD_REG` (`[op:8][reg:8][slot_offset:16]`). Each spilled value
occupies 2 u32 stack slots (one u64 register). Functions that don't need
spilling never emit them.

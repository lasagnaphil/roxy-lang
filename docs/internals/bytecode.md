# Bytecode Format

Roxy uses a 32-bit fixed-width register-based bytecode format.

## Instruction Encoding

All instructions are 32-bit fixed-width with three formats:

```
Format ABC:  [opcode:8][dst:8][src1:8][src2:8]    — 3-operand (arithmetic, comparisons)
Format ABI:  [opcode:8][dst:8][imm16:16]          — immediate (constants, loads)
Format AOFF: [opcode:8][reg:8][offset:16]         — branch/field access (jumps)
```

Helper functions for encoding/decoding:

```cpp
u32 encode_abc(Opcode op, u8 dst, u8 src1, u8 src2);
u32 encode_abi(Opcode op, u8 dst, u16 imm);
u32 encode_aoff(Opcode op, u8 reg, i16 offset);

Opcode decode_opcode(u32 instr);
u8 decode_a(u32 instr);  // dst or reg
u8 decode_b(u32 instr);  // src1
u8 decode_c(u32 instr);  // src2
u16 decode_imm16(u32 instr);
i16 decode_offset(u32 instr);
```

## Calling Convention

```
Arguments:  R0, R1, R2, R3, ... (left to right)
Return:     R0
```

Each function call allocates a new register window from the shared register file.

## Opcode Categories

| Range | Category | Key Opcodes |
|-------|----------|-------------|
| 0x00-0x0F | Constants, Moves | `LOAD_NULL`, `LOAD_TRUE`, `LOAD_FALSE`, `LOAD_INT`, `LOAD_CONST`, `MOV` |
| 0x10-0x1F | Integer Arithmetic | `ADD_I`, `SUB_I`, `MUL_I`, `DIV_I`, `MOD_I`, `NEG_I` |
| 0x20-0x24 | f32 Arithmetic | `ADD_F`, `SUB_F`, `MUL_F`, `DIV_F`, `NEG_F` |
| 0x25-0x29 | f64 Arithmetic | `ADD_D`, `SUB_D`, `MUL_D`, `DIV_D`, `NEG_D` |
| 0x30-0x3F | Bitwise | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `BIT_NOT`, `SHL`, `SHR`, `USHR` |
| 0x40-0x4F | Integer Comparisons | `EQ_I`, `NE_I`, `LT_I`, `LE_I`, `GT_I`, `GE_I`, `LT_U`, `LE_U`, `GT_U`, `GE_U` |
| 0x50-0x55 | f32 Comparisons | `EQ_F`, `NE_F`, `LT_F`, `LE_F`, `GT_F`, `GE_F` |
| 0x56-0x5B | f64 Comparisons | `EQ_D`, `NE_D`, `LT_D`, `LE_D`, `GT_D`, `GE_D` |
| 0x60-0x6F | Logical | `NOT`, `AND`, `OR` |
| 0x80-0x8F | Type Conversions | `I_TO_F64`, `F64_TO_I`, `I_TO_B`, `B_TO_I`, `TRUNC_S`, `TRUNC_U`, `F32_TO_F64`, `F64_TO_F32`, `I_TO_F32`, `F32_TO_I` |
| 0x90-0x9F | Control Flow | `JMP`, `JMP_IF`, `JMP_IF_NOT`, `RET`, `RET_VOID`, `RET_STRUCT_SMALL` |
| 0xA0-0xAF | Function Calls (two-word) + Fused f64 cmp-branch | `CALL`, `CALL_NATIVE`, `JMP_IF_LT_D`, `JMP_IF_LE_D_RK`, ... |
| 0xB0-0xBF | Struct/Stack Access | `GET_FIELD`, `SET_FIELD`, `STACK_ADDR`, `GET_FIELD_ADDR`, `STRUCT_LOAD_REGS`, `STRUCT_STORE_REGS`, `STRUCT_COPY`, `RET_STRUCT_SMALL`, `SPILL_REG`, `RELOAD_REG` |
| 0xC0-0xCF | RK Variants (arith + int cmp) | `ADD_I_RK`, `SUB_I_RK`, `ADD_D_RK`, `MUL_D_RK`, `LT_I_RK`, ... |
| 0xD0-0xDF | Object Lifecycle + f64 cmp RK | `NEW_OBJ`, `DEL_OBJ`, `LT_D_RK`, `GT_D_RK`, ... |
| 0xE0-0xEF | Reference Counting | `REF_INC`, `REF_DEC`, `WEAK_CHECK` |
| 0xFE-0xFF | Debug/Special | `NOP`, `HALT` |

## RK (Register-or-Constant) Encoding

To eliminate `LOAD_INT`/`LOAD_CONST` materialization for the common case of an
arithmetic op or comparison against a compile-time constant (e.g. `i + 1`,
`zx2 + zy2 > 4.0`), each RK-eligible opcode has a parallel `*_RK` variant:

```
Format: [op_RK:8][dst:8][src1:8][const_idx:8]
            // dst = src1 OP K[const_idx]
```

The encoding mirrors ABC, but the `c` field is read as an 8-bit index into the
function's constant pool (`BCFunction::constants`) rather than a register
number. Specialized inline loaders in the interpreter (`rk_const_i64`,
`rk_const_f32`, `rk_const_f64`) bypass the type-switch in `load_constant` —
the lowering pass guarantees that `*_I_RK` opcodes only reference `Int`-typed
constants, `*_D_RK` only `Float`, and `*_F_RK` references `Int` constants
holding f32 bit patterns.

**Lowering** (`compute_const_use_modes` in `lowering.cpp`): a pre-pass walks
the IR and marks each constant SSA value with whether all its uses are
RK-eligible (RHS of any RK op, or either side of a commutative RK op).
Constants flagged "RK-only" skip both register allocation and `LOAD_*`
emission — the RK opcode reads them straight from the constant pool.
Commutative ops (`AddI`, `MulI`, `AddD`, etc.) are canonicalized so the
constant lands on the RHS.

**Why opcode-variant RK over Lua's operand-bit RK**: Lua steals a bit from
each operand field for the K flag, shrinking register space to 128 entries
and forcing aggressive spilling. Opcode-variant RK keeps Roxy's 8-bit
register fields (256 entries) at the cost of more opcodes — still well within
the 256-opcode budget.

**Constant pool index limit**: 8 bits (256 entries). Functions exceeding 256
constants fall back to materialization (the existing `LOAD_CONST` path uses
16-bit `imm16` and is unaffected).

**Integer comparison RK** (`*_I_RK` opcodes): defined but not currently emitted
by the lowering pass. The existing `fuse_compare_branch()` turns `LT_I + JMP_IF_NOT`
into a single `JMP_IF_GE_I` (one dispatch); RK comparisons would lose this
fusion (two dispatches: `LT_I_RK + JMP_IF_NOT`). When `JMP_IF_*_I_RK` fused
variants land, lowering will start emitting integer compare RK.

## Type Conversion Opcodes

| Opcode | Format | Description |
|--------|--------|-------------|
| `I_TO_F64` | ABC | Convert integer to f64 |
| `F64_TO_I` | ABC | Convert f64 to integer (truncate toward zero) |
| `I_TO_B` | ABC | Convert any value to bool (normalize to 0/1) |
| `B_TO_I` | ABC | Convert bool to integer |
| `TRUNC_S` | ABC | Truncate to N bits with sign extension (C = bit width: 8, 16, 32) |
| `TRUNC_U` | ABC | Truncate to N bits with zero extension (C = bit width: 8, 16, 32) |
| `F32_TO_F64` | ABC | Convert f32 to f64 (widening) |
| `F64_TO_F32` | ABC | Convert f64 to f32 (narrowing) |
| `I_TO_F32` | ABC | Convert integer to f32 |
| `F32_TO_I` | ABC | Convert f32 to integer (truncate toward zero) |

## Bytecode Structures

```cpp
// Constant pool entry
struct BCConstant {
    enum Type : u8 { Null, Bool, Int, Float, String };
    Type type;
    union {
        bool as_bool;
        i64 as_int;
        f64 as_float;
        struct { const char* data; u32 length; } as_string;
    };
};

// Bytecode function
struct BCFunction {
    StringView name;
    u32 param_count;
    u32 register_count;
    u32 local_stack_slots;        // Slots needed for local structs
    Vector<u32> code;             // Bytecode instructions
    Vector<BCConstant> constants; // Constant pool
};

// Bytecode module
struct BCModule {
    StringView name;
    Vector<BCFunction*> functions;
    Vector<BCNativeFunction> native_functions;
};
```

## Special Instruction Encodings

### Field Access (Two-Word Instructions)

Field access instructions use two words to encode the slot offset:

```
GET_FIELD: [GET_FIELD dst obj slot_count][slot_offset:16]
SET_FIELD: [SET_FIELD obj val slot_count][slot_offset:16]
STACK_ADDR: [STACK_ADDR dst][slot_offset:16]
```

The `slot_count` (1 or 2) determines whether to read/write 32-bit or 64-bit values.

### Fused Compare-and-Branch

Both integer and f64 comparisons can fuse with the following `JMP_IF`/`JMP_IF_NOT`
into a single two-word instruction:

```
Non-RK: [op:8][_:8][src1:8][src2:8] + [offset:i32]
RK:     [op:8][_:8][src1:8][const_idx:8] + [offset:i32]
```

Fusion runs as a post-emission peephole pass (`fuse_compare_branch`) that
scans for adjacent compare + branch pairs whose registers match, then
substitutes the fused opcode (negating the predicate when the branch is
`JMP_IF_NOT`). Compares whose result is read in another block are tracked
in `m_unfusable_cmp_pcs` and skipped — fusion drops the register write.

Currently fused: `EQ_I`/`NE_I`/`LT_I`/`LE_I`/`GT_I`/`GE_I` (signed integer),
`EQ_D`/`NE_D`/`LT_D`/`LE_D`/`GT_D`/`GE_D` (f64), and the f64 RK variants.
f32 fused branches and integer-RK fused branches are not yet implemented.

### Function Calls (Two-Word Instructions)

`CALL` and `CALL_NATIVE` are two-word instructions to lift the 256-function
ceiling that an 8-bit operand-field func_idx would impose:

```
CALL:        [CALL:8 dst:8 _:8 arg_count:8][func_idx:32]
CALL_NATIVE: [CALL_NATIVE:8 dst:8 _:8 arg_count:8][func_idx:32]
```

Args are passed in registers `dst+ret_reg_count`, `dst+ret_reg_count+1`, ...
(see calling convention above). The full 32-bit func_idx is read directly
into a register-sized temporary; the upper bits are reserved for future
inline-cache slots or tail-call flags.

### Register Spill/Reload

When register pressure exceeds the 255-register limit, the lowering pass spills long-lived values to the local stack. Two single-word ABI-format instructions handle this:

```
SPILL_REG  (0xB8): [SPILL_REG:8][reg:8][slot_offset:16]
    local_stack[base + slot_offset] = regs[reg]   (8 bytes → 2 u32 slots)

RELOAD_REG (0xB9): [RELOAD_REG:8][reg:8][slot_offset:16]
    regs[reg] = local_stack[base + slot_offset]    (2 u32 slots → 8 bytes)
```

Each spilled value occupies 2 u32 stack slots (one u64 register value). Functions that don't require spilling never emit these instructions.

## Files

- `include/roxy/vm/bytecode.hpp` - Opcode definitions, encoding/decoding helpers
- `src/roxy/vm/bytecode.cpp` - Bytecode utilities

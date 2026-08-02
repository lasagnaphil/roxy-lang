# Bytecode Format

Roxy uses a 32-bit fixed-width register-based bytecode format.

## Instruction Encoding

All instructions are 32-bit fixed-width with three formats:

```
Format ABC:  [opcode:8][dst:8][src1:8][src2:8]    — 3-operand (arithmetic, comparisons)
Format ABI:  [opcode:8][dst:8][imm16:16]          — immediate (constants, loads)
Format AOFF: [opcode:8][reg:8][offset:16]         — branch/field access (jumps)
```

Encoding/decoding helpers live in `bytecode.hpp`: `encode_abc` / `encode_abi` /
`encode_aoff`, and `decode_opcode` / `decode_a` (dst or reg) / `decode_b` (src1) /
`decode_c` (src2) / `decode_imm16` / `decode_offset`.

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
| 0x10-0x1F | Integer Arithmetic | `ADD_I`, `SUB_I`, `MUL_I`, `DIV_I`, `MOD_I`, `NEG_I`, `DIV_U`, `MOD_U` |
| 0x20-0x24 | f32 Arithmetic | `ADD_F`, `SUB_F`, `MUL_F`, `DIV_F`, `NEG_F` |
| 0x25-0x29 | f64 Arithmetic | `ADD_D`, `SUB_D`, `MUL_D`, `DIV_D`, `NEG_D` |
| 0x30-0x3F | Bitwise | `BIT_AND`, `BIT_OR`, `BIT_XOR`, `BIT_NOT`, `SHL`, `SHR`, `USHR` |
| 0x40-0x4F | Integer Comparisons | `EQ_I`, `NE_I`, `LT_I`, `LE_I`, `GT_I`, `GE_I`, `LT_U`, `LE_U`, `GT_U`, `GE_U` |
| 0x50-0x55 | f32 Comparisons | `EQ_F`, `NE_F`, `LT_F`, `LE_F`, `GT_F`, `GE_F` |
| 0x56-0x5B | f64 Comparisons | `EQ_D`, `NE_D`, `LT_D`, `LE_D`, `GT_D`, `GE_D` |
| 0x60-0x6F | Logical | `NOT`, `AND`, `OR` |
| 0x80-0x8F | Type Conversions | `I_TO_F64`, `F64_TO_I`, `I_TO_B`, `B_TO_I`, `TRUNC_S`, `TRUNC_U`, `F32_TO_F64`, `F64_TO_F32`, `I_TO_F32`, `F32_TO_I` |
| 0x90-0x9A | Control Flow + Fused int cmp-branch | `JMP`, `JMP_IF`, `JMP_IF_NOT`, `RET`, `RET_VOID`, `JMP_IF_LT_I` … `JMP_IF_NE_I` |
| 0xA0-0xAF | Calls, Container Indexing, Fused f64 cmp-branch | `CALL`, `CALL_NATIVE`, `INDEX_GET_LIST`, `INDEX_SET_LIST`, `INDEX_GET_MAP`, `INDEX_SET_MAP`, `JMP_IF_LT_D` … `JMP_IF_GE_D_RK` |
| 0xB0-0xBF | Struct/Stack/Global Access | `GET_FIELD`, `SET_FIELD`, `STACK_ADDR`, `GET_FIELD_ADDR`, `STRUCT_LOAD_REGS`, `STRUCT_STORE_REGS`, `STRUCT_COPY`, `RET_STRUCT_SMALL`, `SPILL_REG`, `RELOAD_REG`, `STRUCT_COPY_1`–`STRUCT_COPY_4`, `GLOBAL_ADDR`, `RET_WEAK` |
| 0xC0-0xCF | RK Variants (arith + int cmp) | `ADD_I_RK`, `SUB_I_RK`, `ADD_D_RK`, `MUL_D_RK`, `LT_I_RK`, ... |
| 0xD0-0xDF | Object Lifecycle, Exceptions, Closures + f64 cmp RK | `NEW_OBJ`, `DEL_OBJ`, `DELETE`, `THROW`, `CALL_EXC_MSG`, `CALL_INDIRECT`, `ASSERT_HEAP`, `LT_D_RK` … `JMP_IF_NE_D_RK` |
| 0xE0-0xEA | Ref Counting, Element Lvalues, Strings | `REF_INC`, `REF_DEC`, `WEAK_CHECK`, `WEAK_CREATE`, `INDEX_ADDR_LIST`, `INDEX_ADDR_MAP`, `CONTAINER_PIN`, `CONTAINER_UNPIN`, `STR_RETAIN`, `STR_RELEASE`, `INDEX_TRYADDR_MAP` |
| 0xF0, 0xFE-0xFF | Debug/Special | `TRAP`, `NOP`, `HALT` |

`bytecode.hpp` is the authoritative table (152 opcodes); the ranges above are a map, not a listing.

### Returning multi-register values

Three return opcodes, distinguished by *where the value is*:

| Opcode | Value location | Used for |
|---|---|---|
| `RET` | `regs[a]` | everything that fits one register |
| `RET_STRUCT_SMALL` | `*regs[a]` — a **pointer** to ≤4 slots | small structs (a struct value lives in memory) |
| `RET_WEAK` | `regs[a]`, `regs[a+1]` — **inline** | `weak T` = `{pointer, generation}` |

`RET_WEAK` exists because a `weak` is the one multi-register value that is *not*
a pointer to memory: `WEAK_CREATE` writes the pair straight into two registers.
Returning it through `RET_STRUCT_SMALL` dereferenced the pointer half and handed
the caller the pointee's first four slots as the `{pointer, generation}` pair —
garbage the caller then dereferenced, so *every* `weak`-returning function
segfaulted.

The caller side already sized this correctly: `BCFunction::ret_reg_count` is 2
for a `weak` return, and a call's argument window starts at `dst +
ret_reg_count` (not `dst + 1`), so the second return register is never an
argument slot.

## RK (Register-or-Constant) Encoding

To avoid `LOAD_INT`/`LOAD_CONST` materialization when an arithmetic op or
comparison operates against a compile-time constant (e.g. `i + 1`,
`zx2 + zy2 > 4.0`), each RK-eligible opcode has a parallel `*_RK` variant:

```
Format: [op_RK:8][dst:8][src1:8][const_idx:8]
            // dst = src1 OP K[const_idx]
```

The encoding mirrors ABC, but `c` is read as an 8-bit index into the function's
constant pool rather than a register number. Specialized inline loaders
(`rk_const_i64`, `rk_const_f32`, `rk_const_f64`) bypass `load_constant`'s
type-switch — lowering guarantees `*_I_RK` references only `Int` constants,
`*_D_RK` only `Float`, and `*_F_RK` only `Int` constants holding f32 bit
patterns.

**Lowering** (`compute_const_use_modes` in `lowering.cpp`): a pre-pass marks each
constant SSA value with whether all its uses are RK-eligible (RHS of any RK op,
or either side of a commutative RK op). "RK-only" constants skip both register
allocation and `LOAD_*` emission — the RK opcode reads them straight from the
pool. Commutative ops (`AddI`, `MulI`, `AddD`, ...) are canonicalized so the
constant lands on the RHS.

The constant-pool index is 8 bits: functions exceeding **256 constants** fall
back to materialization (the `LOAD_CONST` path uses 16-bit `imm16` and is
unaffected). Opcode-variant RK keeps Roxy's full 8-bit (256-entry) register
fields, at the cost of more opcodes — still within the 256-opcode budget.

**Integer comparison RK** (`*_I_RK`): defined but not currently emitted.
`fuse_compare_branch()` turns `LT_I + JMP_IF_NOT` into a single `JMP_IF_GE_I`
(one dispatch); integer RK comparisons would lose this fusion (two dispatches).
Lowering will emit integer compare RK once `JMP_IF_*_I_RK` fused variants land.

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

Defined in `bytecode.hpp`:

- **`BCConstant`** — a tagged constant-pool entry (`Null`/`Bool`/`Int`/`Float`/`String`) with a union payload.
- **`BCFunction`** — name, `param_count` / `param_register_count`, `register_count`, `ret_reg_count`, `local_stack_slots` (slots for local structs), the `code` instruction vector, the `constants` pool, and the exception-handling side tables: `exception_handlers`, `cleanup_records` ([exceptions.md](exceptions.md)) and `delete_descs` / `struct_field_deletes`, the descriptor tree that drives recursive typed destruction ([recursive-types.md](recursive-types.md)).
- **`BCModule`** — name, the `BCFunction`s and `BCNativeFunction`s, the `types` table (+ registered `type_ids`) used for heap allocation, and `global_slot_count` ([globals.md](globals.md)).

## Special Instruction Encodings

### Field Access (Two-Word Instructions)

Field access encodes the slot offset in a second word:

```
GET_FIELD: [GET_FIELD dst obj slot_count][slot_offset:16]
SET_FIELD: [SET_FIELD obj val slot_count][slot_offset:16]
STACK_ADDR: [STACK_ADDR dst][slot_offset:16]
```

`slot_count` (1 or 2) determines whether to read/write 32-bit or 64-bit values.

### Fused Compare-and-Branch

Both integer and f64 comparisons can fuse with a following `JMP_IF`/`JMP_IF_NOT`
into a single two-word instruction:

```
Non-RK: [op:8][_:8][src1:8][src2:8] + [offset:i32]
RK:     [op:8][_:8][src1:8][const_idx:8] + [offset:i32]
```

Fusion is a post-emission peephole pass (`fuse_compare_branch`) that scans for
adjacent compare + branch pairs with matching registers, then substitutes the
fused opcode (negating the predicate when the branch is `JMP_IF_NOT`). Compares
whose result is read in another block are tracked in `m_unfusable_cmp_pcs` and
skipped — fusion drops the register write.

Currently fused: `EQ_I`/`NE_I`/`LT_I`/`LE_I`/`GT_I`/`GE_I` (signed integer),
`EQ_D`/`NE_D`/`LT_D`/`LE_D`/`GT_D`/`GE_D` (f64), and the f64 RK variants. f32
fused branches and integer-RK fused branches are not yet implemented.

### Function Calls (Two-Word Instructions)

`CALL` and `CALL_NATIVE` are two-word instructions, lifting the 256-function
ceiling an 8-bit operand-field func_idx would impose:

```
CALL:        [CALL:8 dst:8 _:8 arg_count:8][func_idx:32]
CALL_NATIVE: [CALL_NATIVE:8 dst:8 _:8 arg_count:8][func_idx:32]
```

Args are passed in registers `dst+ret_reg_count`, `dst+ret_reg_count+1`, ... (see
calling convention). The full 32-bit func_idx is read into a register-sized
temporary; upper bits are reserved for future inline-cache slots or tail-call
flags.

### Register Spill/Reload

When register pressure exceeds the 255-register limit, lowering spills
long-lived values to the local stack via two single-word ABI instructions:

```
SPILL_REG  (0xB8): [SPILL_REG:8][reg:8][slot_offset:16]
    local_stack[base + slot_offset] = regs[reg]   (8 bytes → 2 u32 slots)

RELOAD_REG (0xB9): [RELOAD_REG:8][reg:8][slot_offset:16]
    regs[reg] = local_stack[base + slot_offset]    (2 u32 slots → 8 bytes)
```

Each spilled value occupies 2 u32 stack slots (one u64 register value).
Functions that don't require spilling never emit these instructions.

## Files

| File | Purpose |
|---|---|
| `include/roxy/vm/bytecode.hpp` | Opcode definitions, encoding/decoding helpers, `BCConstant`/`BCFunction`/`BCModule` |
| `src/roxy/vm/bytecode.cpp` | Bytecode utilities |

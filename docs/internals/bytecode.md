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
| 0xA0-0xAF | Function Calls | `CALL`, `CALL_NATIVE` |
| 0xB0-0xBF | Struct Access | `GET_FIELD`, `SET_FIELD`, `STACK_ADDR`, `GET_FIELD_ADDR`, `STRUCT_LOAD_REGS`, `STRUCT_STORE_REGS`, `STRUCT_COPY` |
| 0xC0-0xCF | Index Access | `GET_INDEX`, `SET_INDEX` |
| 0xD0-0xDF | Object Lifecycle | `NEW_OBJ`, `DEL_OBJ` |
| 0xE0-0xEF | Reference Counting | `REF_INC`, `REF_DEC`, `WEAK_CHECK` |
| 0xFE-0xFF | Debug/Special | `NOP`, `HALT` |

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

## Files

- `include/roxy/vm/bytecode.hpp` - Opcode definitions, encoding/decoding helpers
- `src/roxy/vm/bytecode.cpp` - Bytecode utilities

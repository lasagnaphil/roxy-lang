#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/unique_ptr.hpp"

namespace rx {

// Bytecode opcodes - 8-bit opcode in fixed 32-bit instruction format
//
// Instruction formats:
//   Format A: [opcode:8][dst:8][src1:8][src2:8] - 3-operand
//   Format B: [opcode:8][dst:8][imm16:16]       - immediate/offset
//   Format C: [opcode:8][reg:8][offset:16]      - branch/field access
//
enum class Opcode : u8 {
    // 0x00-0x0F: Constants and Moves
    LOAD_NULL   = 0x00,     // dst = null
    LOAD_TRUE   = 0x01,     // dst = true
    LOAD_FALSE  = 0x02,     // dst = false
    LOAD_INT    = 0x03,     // dst = imm16 (sign-extended)
    LOAD_CONST  = 0x04,     // dst = constants[imm16]
    MOV         = 0x05,     // dst = src

    // 0x10-0x1F: Integer Arithmetic
    ADD_I       = 0x10,     // dst = src1 + src2
    SUB_I       = 0x11,     // dst = src1 - src2
    MUL_I       = 0x12,     // dst = src1 * src2
    DIV_I       = 0x13,     // dst = src1 / src2
    MOD_I       = 0x14,     // dst = src1 % src2
    NEG_I       = 0x15,     // dst = -src1

    // 0x20-0x24: f32 Arithmetic (F = float)
    ADD_F       = 0x20,     // dst = src1 + src2 (f32)
    SUB_F       = 0x21,     // dst = src1 - src2 (f32)
    MUL_F       = 0x22,     // dst = src1 * src2 (f32)
    DIV_F       = 0x23,     // dst = src1 / src2 (f32)
    NEG_F       = 0x24,     // dst = -src1 (f32)

    // 0x25-0x29: f64 Arithmetic (D = double)
    ADD_D       = 0x25,     // dst = src1 + src2 (f64)
    SUB_D       = 0x26,     // dst = src1 - src2 (f64)
    MUL_D       = 0x27,     // dst = src1 * src2 (f64)
    DIV_D       = 0x28,     // dst = src1 / src2 (f64)
    NEG_D       = 0x29,     // dst = -src1 (f64)

    // 0x30-0x3F: Bitwise Operations
    BIT_AND     = 0x30,     // dst = src1 & src2
    BIT_OR      = 0x31,     // dst = src1 | src2
    BIT_XOR     = 0x32,     // dst = src1 ^ src2
    BIT_NOT     = 0x33,     // dst = ~src1
    SHL         = 0x34,     // dst = src1 << src2
    SHR         = 0x35,     // dst = src1 >> src2 (arithmetic)
    USHR        = 0x36,     // dst = src1 >>> src2 (logical)

    // 0x40-0x4F: Integer Comparisons
    EQ_I        = 0x40,     // dst = src1 == src2
    NE_I        = 0x41,     // dst = src1 != src2
    LT_I        = 0x42,     // dst = src1 < src2 (signed)
    LE_I        = 0x43,     // dst = src1 <= src2 (signed)
    GT_I        = 0x44,     // dst = src1 > src2 (signe )
    GE_I        = 0x45,     // dst = src1 >= src2 (signed)
    LT_U        = 0x46,     // dst = src1 < src2 (unsigned)
    LE_U        = 0x47,     // dst = src1 <= src2 (unsigned)
    GT_U        = 0x48,     // dst = src1 > src2 (unsigned)
    GE_U        = 0x49,     // dst = src1 >= src2 (unsigned)

    // 0x50-0x55: f32 Comparisons (F = float)
    EQ_F        = 0x50,     // dst = src1 == src2 (f32)
    NE_F        = 0x51,     // dst = src1 != src2 (f32)
    LT_F        = 0x52,     // dst = src1 < src2 (f32)
    LE_F        = 0x53,     // dst = src1 <= src2 (f32)
    GT_F        = 0x54,     // dst = src1 > src2 (f32)
    GE_F        = 0x55,     // dst = src1 >= src2 (f32)

    // 0x56-0x5B: f64 Comparisons (D = double)
    EQ_D        = 0x56,     // dst = src1 == src2 (f64)
    NE_D        = 0x57,     // dst = src1 != src2 (f64)
    LT_D        = 0x58,     // dst = src1 < src2 (f64)
    LE_D        = 0x59,     // dst = src1 <= src2 (f64)
    GT_D        = 0x5A,     // dst = src1 > src2 (f64)
    GE_D        = 0x5B,     // dst = src1 >= src2 (f64)

    // 0x60-0x6F: Logical Operations
    NOT         = 0x60,     // dst = !src1
    AND         = 0x61,     // dst = src1 && src2 (not short-circuit in bytecode)
    OR          = 0x62,     // dst = src1 || src2 (not short-circuit in bytecode)

    // 0x80-0x8F: Type Conversions
    I_TO_F64    = 0x80,     // dst = (f64)src - integer to f64
    F64_TO_I    = 0x81,     // dst = (i64)src - f64 to integer (truncate toward zero)
    I_TO_B      = 0x82,     // dst = (bool)src - any value to bool (normalize to 0/1)
    B_TO_I      = 0x83,     // dst = (int)src - bool to int
    TRUNC_S     = 0x84,     // dst = truncate src to N bits, sign-extend back
                            // Format: [TRUNC_S:8][dst:8][src:8][bits:8] (bits: 8, 16, or 32)
    TRUNC_U     = 0x85,     // dst = truncate src to N bits, zero-extend back
                            // Format: [TRUNC_U:8][dst:8][src:8][bits:8]
    F32_TO_F64  = 0x86,     // dst = (f64)src where src is f32
    F64_TO_F32  = 0x87,     // dst = (f32)src where src is f64
    I_TO_F32    = 0x88,     // dst = (f32)src where src is integer
    F32_TO_I    = 0x89,     // dst = (i64)src where src is f32 (truncate toward zero)

    // 0x90-0x9F: Control Flow
    JMP         = 0x90,     // pc += offset (signed 16-bit)
    JMP_IF      = 0x91,     // if (reg) pc += offset
    JMP_IF_NOT  = 0x92,     // if (!reg) pc += offset
    RET         = 0x93,     // return reg (or void if reg=0xFF)
    RET_VOID    = 0x94,     // return (void)

    // 0xA0-0xAF: Function Calls and Container Indexing
    CALL        = 0xA0,     // dst = call func_idx(args...)
    CALL_NATIVE = 0xA1,     // dst = call_native func_idx(args...)
    INDEX_GET_LIST = 0xA2,  // dst = list[index]     — ABC: a=dst, b=obj, c=index
    INDEX_SET_LIST = 0xA3,  // list[index] = value    — ABC: a=obj, b=index, c=value
    INDEX_GET_MAP  = 0xA4,  // dst = map[key]         — ABC: a=dst, b=obj, c=key
    INDEX_SET_MAP  = 0xA5,  // map[key] = value       — ABC: a=obj, b=key, c=value

    // 0xB0-0xBF: Field and Stack Access
    GET_FIELD       = 0xB0, // dst = src1.field[slot_offset] (two-word: ABC + offset)
    SET_FIELD       = 0xB1, // dst.field[slot_offset] = src1 (two-word: ABC + offset)
    STACK_ADDR      = 0xB2, // dst = &local_stack[local_stack_base + imm16]
    GET_FIELD_ADDR  = 0xB3, // dst = &src1.field[slot_offset] (two-word: ABI format)
    STRUCT_LOAD_REGS  = 0xB4, // dst = *src (load struct to consecutive registers)
    STRUCT_STORE_REGS = 0xB5, // *dst = src (store consecutive registers to struct)
    STRUCT_COPY       = 0xB6, // [dst_ptr src_ptr slot_count] - memory copy
    RET_STRUCT_SMALL  = 0xB7, // return small struct (≤4 slots) in registers
    SPILL_REG         = 0xB8, // spill regs[reg] to local_stack[base + imm16] (2 u32 slots)
    RELOAD_REG        = 0xB9, // reload regs[reg] from local_stack[base + imm16] (2 u32 slots)

    // 0xD0-0xDF: Object Lifecycle
    NEW_OBJ     = 0xD0,     // dst = new type[imm16]
    DEL_OBJ     = 0xD1,     // delete reg

    // 0xE0-0xEF: Reference Counting
    REF_INC     = 0xE0,     // ref_inc(reg)
    REF_DEC     = 0xE1,     // ref_dec(reg)
    WEAK_CHECK  = 0xE2,     // dst = weak_valid(src1)
    WEAK_CREATE = 0xE3,     // dst,dst+1 = weak_create(src) — extracts generation

    // 0xD2-0xD3: Exception Handling
    THROW       = 0xD2,     // throw regs[a] (exception object pointer)
    CALL_EXC_MSG = 0xD3,    // dst = exception_message(regs[src]) - call stored message fn ptr

    // 0xF0-0xFD: Debug/Error
    TRAP        = 0xF0,     // runtime error trap (for variant field access checks)

    // 0xFF: Invalid/Debug
    NOP         = 0xFE,     // no operation
    HALT        = 0xFF,     // halt execution
};

// Get string representation of opcode
const char* opcode_to_string(Opcode op);

// Instruction encoding helpers
// Format A: [opcode:8][dst:8][src1:8][src2:8]
inline u32 encode_abc(Opcode op, u8 dst, u8 src1, u8 src2) {
    return (static_cast<u32>(op) << 24) | (static_cast<u32>(dst) << 16) |
           (static_cast<u32>(src1) << 8) | static_cast<u32>(src2);
}

// Format B: [opcode:8][dst:8][imm16:16]
inline u32 encode_abi(Opcode op, u8 dst, u16 imm) {
    return (static_cast<u32>(op) << 24) | (static_cast<u32>(dst) << 16) | static_cast<u32>(imm);
}

// Format C: [opcode:8][reg:8][offset:16]
inline u32 encode_aoff(Opcode op, u8 reg, i16 offset) {
    return (static_cast<u32>(op) << 24) | (static_cast<u32>(reg) << 16) |
           static_cast<u32>(static_cast<u16>(offset));
}

// Instruction decoding helpers
inline Opcode decode_opcode(u32 instr) {
    return static_cast<Opcode>((instr >> 24) & 0xFF);
}

inline u8 decode_a(u32 instr) {
    return static_cast<u8>((instr >> 16) & 0xFF);
}

inline u8 decode_b(u32 instr) {
    return static_cast<u8>((instr >> 8) & 0xFF);
}

inline u8 decode_c(u32 instr) {
    return static_cast<u8>(instr & 0xFF);
}

inline u16 decode_imm16(u32 instr) {
    return static_cast<u16>(instr & 0xFFFF);
}

inline i16 decode_offset(u32 instr) {
    return static_cast<i16>(instr & 0xFFFF);
}

// Constant pool value
struct BCConstant {
    enum Type : u8 {
        Null,
        Bool,
        Int,
        Float,
        String,
    };

    Type type;
    union {
        bool as_bool;
        i64 as_int;
        f64 as_float;
        struct {
            const char* data;
            u32 length;
        } as_string;
    };

    BCConstant() : type(Null), as_int(0) {}

    static BCConstant make_null() {
        BCConstant c;
        c.type = Null;
        return c;
    }

    static BCConstant make_bool(bool v) {
        BCConstant c;
        c.type = Bool;
        c.as_bool = v;
        return c;
    }

    static BCConstant make_int(i64 v) {
        BCConstant c;
        c.type = Int;
        c.as_int = v;
        return c;
    }

    static BCConstant make_float(f64 v) {
        BCConstant c;
        c.type = Float;
        c.as_float = v;
        return c;
    }

    static BCConstant make_string(const char* data, u32 length) {
        BCConstant c;
        c.type = String;
        c.as_string.data = data;
        c.as_string.length = length;
        return c;
    }
};

// Type info for heap allocation - maps type_idx to allocation info
struct BCTypeInfo {
    StringView name;      // Struct name
    u32 size_bytes;       // Size for object_alloc
    u32 slot_count;       // For field access
};

// Exception handler entry in bytecode
struct BCExceptionHandler {
    u32 try_start_pc;     // Protected range start (inclusive, offset into code[])
    u32 try_end_pc;       // Protected range end (exclusive)
    u32 handler_pc;       // Catch handler entry point
    u32 type_id;          // Concrete type_id to match (0 = catch-all)
    u8 exception_reg;     // Register to store exception ptr in handler
};

// Cleanup record for exception-path cleanup of owned locals
// During exception handling, the VM iterates these in reverse (LIFO) to clean up
// owned variables whose scope spans the throw site but not the handler site.
struct BCCleanupRecord {
    u32 scope_start_pc;       // PC where variable becomes live (inclusive)
    u32 scope_end_pc;         // PC where variable's normal cleanup occurs (exclusive)
    u8 register_idx;          // Register holding the owned value
    u8 kind;                  // 0=DEL_OBJ, 1=CALL_DTOR+DEL_OBJ, 2=CALL_DTOR, 3=LIST_CLEANUP, 4=MAP_CLEANUP
    u16 destructor_fn_idx;    // Function index for destructor (kinds 1, 2 only)
};

// Bytecode function
struct BCFunction {
    StringView name;            // Function name
    u32 param_count;            // Number of parameters
    u32 param_register_count;   // Total registers needed for parameters (may exceed param_count for multi-register structs)
    u32 register_count;         // Total registers needed
    u32 local_stack_slots;      // Local stack slots needed for struct data
    Vector<u32> code;           // Bytecode instructions
    Vector<BCConstant> constants; // Constant pool
    Vector<BCExceptionHandler> exception_handlers; // Exception handler table
    Vector<BCCleanupRecord> cleanup_records;        // Cleanup records for exception handling

    BCFunction() : param_count(0), param_register_count(0), register_count(0), local_stack_slots(0) {}
};

// Native function signature
using NativeFunction = void(*)(struct RoxyVM* vm, u8 dst_reg, u8 arg_count, u8 first_arg_reg);

// Native function entry
struct BCNativeFunction {
    StringView name;            // Function name
    NativeFunction func;        // Native function pointer
    u32 param_count;            // Number of parameters

    BCNativeFunction() : func(nullptr), param_count(0) {}
};

// Bytecode module - collection of functions
struct BCModule {
    StringView name;                        // Module name
    Vector<UniquePtr<BCFunction>> functions; // User-defined functions
    Vector<BCNativeFunction> native_functions; // Native functions
    Vector<BCTypeInfo> types;               // Type table for heap allocation
    Vector<u32> type_ids;                   // Global type IDs after registration

    BCModule() = default;
    ~BCModule() = default;

    // Find function by name, returns index or -1 if not found
    i32 find_function(StringView name) const {
        for (u32 i = 0; i < functions.size(); i++) {
            if (functions[i]->name == name) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }

    // Find native function by name, returns index or -1 if not found
    i32 find_native_function(StringView name) const {
        for (u32 i = 0; i < native_functions.size(); i++) {
            if (native_functions[i].name == name) {
                return static_cast<i32>(i);
            }
        }
        return -1;
    }
};

// Disassemble a single instruction
void disassemble_instruction(u32 instr, u32 offset, String& out);

// Disassemble a function
void disassemble_function(const BCFunction* func, String& out);

// Disassemble a module
void disassemble_module(const BCModule* module, String& out);

}

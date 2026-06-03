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
    // Note: no AND/OR opcodes — Roxy's bool representation is normalized 0/1
    // (LOAD_TRUE/FALSE, comparison ops, NOT, I_TO_B all produce 0 or 1), so
    // BIT_AND/BIT_OR are bit-equivalent to && / ||. Source-level && and ||
    // lower to short-circuit branches in the IR builder. Lua follows the same
    // pattern. Slots 0x61-0x62 are free for future logical ops.
    NOT         = 0x60,     // dst = !src1

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

    // Fused compare-and-branch (two-word: [op:8][_:8][src1:8][src2:8] + [offset:32])
    JMP_IF_LT_I = 0x95,    // if (src1 <  src2) pc += offset (signed i32)
    JMP_IF_LE_I = 0x96,    // if (src1 <= src2) pc += offset (signed i32)
    JMP_IF_GT_I = 0x97,    // if (src1 >  src2) pc += offset (signed i32)
    JMP_IF_GE_I = 0x98,    // if (src1 >= src2) pc += offset (signed i32)
    JMP_IF_EQ_I = 0x99,    // if (src1 == src2) pc += offset (signed i32)
    JMP_IF_NE_I = 0x9A,    // if (src1 != src2) pc += offset (signed i32)

    // 0xA0-0xAF: Function Calls and Container Indexing
    // CALL/CALL_NATIVE are two-word: word 1 = [op:8][dst:8][_:8][arg_count:8],
    // word 2 = [func_idx:32]. The 32-bit func_idx removes the 256-function ceiling
    // a packed 8-bit b-field would impose; in practice modules with thousands of
    // functions (e.g. linked-together game scripts) need the headroom.
    CALL        = 0xA0,     // dst = call func_idx(args...) — two-word
    CALL_NATIVE = 0xA1,     // dst = call_native func_idx(args...) — two-word
    INDEX_GET_LIST = 0xA2,  // dst = list[index]     — ABC: a=dst, b=obj, c=index
    INDEX_SET_LIST = 0xA3,  // list[index] = value    — ABC: a=obj, b=index, c=value
    INDEX_GET_MAP  = 0xA4,  // dst = map[key]         — ABC: a=dst, b=obj, c=key
    INDEX_SET_MAP  = 0xA5,  // map[key] = value       — ABC: a=obj, b=key, c=value

    // 0xA6-0xAF: Fused f64 compare-and-branch.
    //   Non-RK: [op:8][_:8][src1:8][src2:8] + [offset:i32]
    //   RK:     [op:8][_:8][src1:8][const_idx:8] + [offset:i32]
    // Saves the JMP_IF_NOT dispatch + register write that would otherwise follow
    // a standalone compare. Mandelbrot's `if (zx2 + zy2 > 4.0)` collapses from
    // (ADD_D + GT_D_RK + JMP_IF_NOT) to (ADD_D + JMP_IF_LE_D_RK).
    JMP_IF_LT_D = 0xA6,     // if (src1 <  src2) pc += offset
    JMP_IF_LE_D = 0xA7,     // if (src1 <= src2) pc += offset
    JMP_IF_GT_D = 0xA8,     // if (src1 >  src2) pc += offset
    JMP_IF_GE_D = 0xA9,     // if (src1 >= src2) pc += offset
    JMP_IF_EQ_D = 0xAA,     // if (src1 == src2) pc += offset
    JMP_IF_NE_D = 0xAB,     // if (src1 != src2) pc += offset
    JMP_IF_LT_D_RK = 0xAC,  // if (src1 <  K[c]) pc += offset
    JMP_IF_LE_D_RK = 0xAD,  // if (src1 <= K[c]) pc += offset
    JMP_IF_GT_D_RK = 0xAE,  // if (src1 >  K[c]) pc += offset
    JMP_IF_GE_D_RK = 0xAF,  // if (src1 >= K[c]) pc += offset

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

    // Specialized STRUCT_COPY for small slot counts. Same ABC encoding as
    // STRUCT_COPY but slot_count is implicit in the opcode, eliminating the
    // per-call loop test/branch. Common struct shapes (Vec2 = 2 slots, Vec3 =
    // 3 slots, Color = 4 slots, single i32 = 1 slot) all fit. Lowering picks
    // these when slot_count ∈ [1, 4]; STRUCT_COPY remains for larger structs.
    STRUCT_COPY_1     = 0xBA, // dst[0] = src[0]
    STRUCT_COPY_2     = 0xBB, // dst[0..2] = src[0..2]
    STRUCT_COPY_3     = 0xBC, // dst[0..3] = src[0..3]
    STRUCT_COPY_4     = 0xBD, // dst[0..4] = src[0..4]

    // 0xC0-0xDA: RK (register-or-constant) variants. Same ABC encoding as the
    // base opcode, but `c` is a constant pool index (u8) instead of a register.
    // Saves the LOAD_INT/LOAD_CONST + register op pair when the RHS is a
    // compile-time constant. Lowering canonicalizes commutative ops so the
    // constant lands on the RHS. Constant pool index >255 falls back to
    // materialization. See docs/internals/bytecode.md.
    ADD_I_RK    = 0xC0,     // dst = src1 + K[c]
    SUB_I_RK    = 0xC1,     // dst = src1 - K[c]
    MUL_I_RK    = 0xC2,     // dst = src1 * K[c]
    ADD_F_RK    = 0xC3,     // dst = src1 + K[c] (f32)
    SUB_F_RK    = 0xC4,     // dst = src1 - K[c] (f32)
    MUL_F_RK    = 0xC5,     // dst = src1 * K[c] (f32)
    ADD_D_RK    = 0xC6,     // dst = src1 + K[c] (f64)
    SUB_D_RK    = 0xC7,     // dst = src1 - K[c] (f64)
    MUL_D_RK    = 0xC8,     // dst = src1 * K[c] (f64)
    DIV_D_RK    = 0xC9,     // dst = src1 / K[c] (f64)
    EQ_I_RK     = 0xCA,     // dst = src1 == K[c]
    NE_I_RK     = 0xCB,     // dst = src1 != K[c]
    LT_I_RK     = 0xCC,     // dst = src1 <  K[c] (signed)
    LE_I_RK     = 0xCD,     // dst = src1 <= K[c] (signed)
    GT_I_RK     = 0xCE,     // dst = src1 >  K[c] (signed)
    GE_I_RK     = 0xCF,     // dst = src1 >= K[c] (signed)

    // 0xD0-0xDF: Object Lifecycle
    NEW_OBJ     = 0xD0,     // dst = new type[imm16]
    DEL_OBJ     = 0xD1,     // delete reg

    // 0xE0-0xEF: Reference Counting
    REF_INC     = 0xE0,     // ref_inc(reg)
    REF_DEC     = 0xE1,     // ref_dec(reg)
    WEAK_CHECK  = 0xE2,     // dst = weak_valid(src1)
    WEAK_CREATE = 0xE3,     // dst,dst+1 = weak_create(src) — extracts generation

    // 0xD2-0xD4: Exception Handling and Typed Delete
    THROW       = 0xD2,     // throw regs[a] (exception object pointer)
    CALL_EXC_MSG = 0xD3,    // dst = exception_message(regs[src]) - call stored message fn ptr
    DELETE      = 0xD4,     // ABI: typed delete regs[a] using delete_descs[imm16]

    // 0xD5-0xDA: f64 comparison RK variants (see RK comment above)
    EQ_D_RK     = 0xD5,     // dst = src1 == K[c] (f64)
    NE_D_RK     = 0xD6,     // dst = src1 != K[c] (f64)
    LT_D_RK     = 0xD7,     // dst = src1 <  K[c] (f64)
    LE_D_RK     = 0xD8,     // dst = src1 <= K[c] (f64)
    GT_D_RK     = 0xD9,     // dst = src1 >  K[c] (f64)
    GE_D_RK     = 0xDA,     // dst = src1 >= K[c] (f64)

    // 0xDB-0xDC: f64 fused-RK compare-and-branch (extension of the 0xA6-0xAF
    // block; placed here because 0xB0-0xCF were already taken).
    JMP_IF_EQ_D_RK = 0xDB,  // if (src1 == K[c]) pc += offset
    JMP_IF_NE_D_RK = 0xDC,  // if (src1 != K[c]) pc += offset

    // 0xDD: Indirect call (closures and first-class function values).
    // dst = call closure(args...) — two-word
    //   word 0: [CALL_INDIRECT][dst][closure_reg][arg_count]
    //   word 1: [reserved:32]   (future inline-cache slot)
    // The closure value is a uniq pointer to a heap-allocated env struct whose
    // first u32 field holds the target function index. The interpreter reads
    // that field, sets up the call frame with the env pointer as the first
    // argument, copies the explicit args after it, and dispatches.
    CALL_INDIRECT = 0xDD,

    // 0xDE: Trap if pointer in regs[a] is not owned by the slab allocator.
    // Used for self captures of copyable structs where the receiver may be
    // stack-allocated. Single-word: [ASSERT_HEAP][a][_][_].
    ASSERT_HEAP = 0xDE,

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
            // Cached StringObject pointer — populated once at vm_load_module.
            // LOAD_CONST returns this directly, skipping the intern-table probe
            // on the hot path. nullptr until the module is loaded.
            void* obj;
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
        c.as_string.obj = nullptr;
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

// One owned-field cleanup action for descriptor-driven struct destruction
// (BCDeleteDesc::WalkFields). Flattens a struct's regular owned fields and its
// tagged-union variant fields into a single guarded list. For variant fields,
// the action only fires when the discriminant slot equals `disc_value`. This
// is the data-driven equivalent of the bytecode emitted by emit_field_cleanup,
// letting the runtime clean up struct fields directly in C++ without
// re-entering the interpreter (which previously caused unbounded native-stack
// recursion when destroying deep recursive structures like linked lists).
//
// Whether the field is an embedded value struct (cleaned in place) or a heap
// pointer (loaded, then deleted) is not stored here — it is exactly the field
// descriptor's `free_obj` flag (a heap pointer is the thing that gets freed).
struct BCStructFieldDelete {
    i32 disc_value;       // variant guard value (valid only if disc_slot_offset != 0xFFFF);
                          // discriminants are enum-backed i32
    u16 slot_offset;      // field's slot offset within the struct (in u32 slots)
    u16 field_desc_idx;   // delete descriptor index for the field's value type
    u16 disc_slot_offset; // discriminant slot offset; 0xFFFF = unconditional (regular field)
    u16 _pad;
};

// Describes how to delete one noncopyable value. Forms a tree via indices into
// BCFunction::delete_descs[] for recursive container cleanup. Used by both
// the DELETE opcode (normal scope-exit) and exception unwinding.
//
// Two orthogonal axes:
//   - `cleanup`: how to recursively release the value's owned resources.
//   - `free_obj`: whether `ptr` is itself a heap allocation to object_free()
//     afterward (true for uniq/list/map/coro pointers, false for embedded
//     value structs). delete_value() applies it as a single trailing step.
//
// Each cleanup uses only its relevant payload, so they share storage via a
// union (the descriptor is 6 bytes instead of 12).
struct BCDeleteDesc {
    enum Cleanup : u8 {
        None,        // no owned resources (e.g. uniq of a primitive)
        CallDtor,    // run the type's bytecode destructor (user/inherited dtor)
        WalkFields,  // walk owned fields directly via struct_field_deletes
        List,        // iterate elements + recurse, free element buffer
        Map,         // iterate occupied buckets + recurse, free bucket buffers
    };

    Cleanup cleanup;
    bool free_obj;              // object_free(ptr) after `cleanup` runs
    union {
        u16 dtor_fn_idx;        // CallDtor: destructor function index
        struct {                // List, Map
            u16 elem_desc_idx;  //   List: element descriptor; Map: value descriptor (0xFFFF = n/a)
            u16 key_desc_idx;   //   Map: key descriptor (0xFFFF = copyable keys)
        } container;
        struct {                // WalkFields
            u16 field_start;    //   start index into BCFunction::struct_field_deletes
            u16 field_count;    //   number of field-cleanup actions
        } fields;
    };

    BCDeleteDesc() : cleanup(None), free_obj(false), dtor_fn_idx(0) {}
};

// Cleanup record for exception-path cleanup of owned locals.
// During exception unwinding, the VM iterates these in reverse (LIFO) to
// delete owned variables whose scope spans the throw site but not the handler.
struct BCCleanupRecord {
    u32 scope_start_pc;       // PC where variable becomes live (inclusive)
    u32 scope_end_pc;         // PC where variable's normal cleanup occurs (exclusive)
    u8 register_idx;          // Register holding the owned value
    u8 _pad;
    u16 delete_desc_idx;      // Index into BCFunction::delete_descs[]
};

// Bytecode function
struct BCFunction {
    StringView name;            // Function name
    u32 param_count;            // Number of parameters
    u32 param_register_count;   // Total registers needed for parameters (may exceed param_count for multi-register structs)
    u32 register_count;         // Total registers needed
    u32 local_stack_slots;      // Local stack slots needed for struct data
    u8 ret_reg_count;           // Registers used by return value (1 normally, 2 for 3-4 slot struct returns)
    Vector<u32> code;           // Bytecode instructions
    Vector<BCConstant> constants; // Constant pool
    Vector<BCExceptionHandler> exception_handlers; // Exception handler table
    Vector<BCCleanupRecord> cleanup_records;        // Cleanup records for exception handling
    Vector<BCDeleteDesc> delete_descs;             // Typed delete descriptors (tree via indices)
    Vector<BCStructFieldDelete> struct_field_deletes; // Field-cleanup actions for STRUCT descriptors (kinds 5/6)

    BCFunction() : param_count(0), param_register_count(0), register_count(0), local_stack_slots(0), ret_reg_count(1) {}
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

// Disassemble a single instruction (may consume 1 or 2 words).
// next_word is the following word in the code stream (for 2-word instructions).
// Returns the number of words consumed (1 or 2).
u32 disassemble_instruction(u32 instr, u32 next_word, u32 offset, String& out);

// Disassemble a function
void disassemble_function(const BCFunction* func, String& out);

// Disassemble a module
void disassemble_module(const BCModule* module, String& out);

}

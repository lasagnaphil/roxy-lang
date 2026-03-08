#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/list.hpp"
#include "roxy/vm/map.hpp"
#include "roxy/vm/string.hpp"

#include <cmath>
#include <cassert>
#include <cstring>

namespace rx {

// Helper functions for type punning with untyped registers
inline i64 reg_as_i64(u64 r) { return static_cast<i64>(r); }
inline f64 reg_as_f64(u64 r) { f64 v; memcpy(&v, &r, sizeof(v)); return v; }
inline void* reg_as_ptr(u64 r) { return reinterpret_cast<void*>(r); }

// f32 helper functions - f32 values are stored as bit patterns in lower 32 bits
inline f32 reg_as_f32(u64 r) {
    u32 bits = static_cast<u32>(r);
    f32 v;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

inline u64 reg_from_i64(i64 v) { return static_cast<u64>(v); }
inline u64 reg_from_f64(f64 v) { u64 r; memcpy(&r, &v, sizeof(r)); return r; }
inline u64 reg_from_f32(f32 v) {
    u32 bits;
    memcpy(&bits, &v, sizeof(bits));
    return static_cast<u64>(bits);
}
inline u64 reg_from_ptr(void* p) { return reinterpret_cast<u64>(p); }
inline u64 reg_from_bool(bool b) { return b ? 1 : 0; }
inline bool reg_is_truthy(u64 r) { return r != 0; }

// Helper to load constant from constant pool into a u64 register
static u64 load_constant(RoxyVM* vm, const BCFunction* func, u16 index) {
    if (index >= func->constants.size()) {
        return 0;
    }

    const BCConstant& c = func->constants[index];
    switch (c.type) {
        case BCConstant::Null:
            return 0;
        case BCConstant::Bool:
            return c.as_bool ? 1 : 0;
        case BCConstant::Int:
            return reg_from_i64(c.as_int);
        case BCConstant::Float:
            return reg_from_f64(c.as_float);
        case BCConstant::String:
            // Create a StringObject from the constant string data
            return reg_from_ptr(string_alloc(vm, c.as_string.data, c.as_string.length));
        default:
            return 0;
    }
}

// Call a destructor function on an object during exception cleanup.
// Uses nested interpretation: pushes a call frame for the destructor,
// runs the interpreter until it returns, then continues.
static void call_cleanup_destructor(RoxyVM* vm, u16 func_idx, void* obj_ptr) {
    if (!obj_ptr) return;
    assert(func_idx < vm->function_count);

    const BCFunction* dtor_func = vm->function_ptrs[func_idx];
    if (!dtor_func) return;

    // Check call stack depth limit
    if (vm->call_stack_size >= vm->call_stack_capacity) return;

    // Allocate registers for the destructor call
    u32 reg_base = vm->register_top;
    if (reg_base + dtor_func->register_count > vm->register_file_size) return;
    vm->register_top += dtor_func->register_count;

    // Zero-initialize the register window
    u64* dtor_regs = &vm->register_file[reg_base];
    memset(dtor_regs, 0, dtor_func->register_count * sizeof(u64));

    // Set up the self parameter (first argument = object pointer)
    dtor_regs[0] = reg_from_ptr(obj_ptr);

    // Allocate local stack space
    u32 local_stack_base = vm->local_stack_top;
    vm->local_stack_top += dtor_func->local_stack_slots;

    // Push call frame
    u32 saved_depth = vm->call_stack_size;
    vm->call_stack[vm->call_stack_size++] = CallFrame(
        dtor_func, dtor_func->code.data(), dtor_regs, 0, local_stack_base);

    // Run the destructor via nested interpretation
    interpret(vm, saved_depth);
}

// Execute cleanup for owned locals during exception handling.
// Iterates cleanup records in reverse (LIFO order) and cleans up any
// variable whose scope spans the throw site but not the handler site.
static void execute_cleanup(RoxyVM* vm, const BCFunction* func,
                            u32 throw_pc, u32 handler_pc_or_max, u64* regs) {
    if (func->cleanup_records.empty()) return;

    // Iterate in reverse for LIFO cleanup order
    for (i32 i = static_cast<i32>(func->cleanup_records.size()) - 1; i >= 0; i--) {
        const BCCleanupRecord& record = func->cleanup_records[i];

        // Check if the throw site is within this variable's live range
        if (throw_pc < record.scope_start_pc || throw_pc >= record.scope_end_pc) {
            continue;  // Throw is outside this variable's scope
        }

        // Check if the handler is also within this variable's scope
        // If so, normal-path cleanup will handle it (handler is still in scope)
        if (handler_pc_or_max >= record.scope_start_pc &&
            handler_pc_or_max < record.scope_end_pc) {
            continue;  // Handler is in scope - normal cleanup will handle this
        }

        // Read the register value
        void* ptr = reg_as_ptr(regs[record.register_idx]);
        if (!ptr) {
            continue;  // Already cleaned up (null)
        }

        switch (record.kind) {
            case 0:  // DEL_OBJ only (uniq without struct destructor)
                object_free(vm, ptr);
                break;

            case 1:  // CALL_DTOR + DEL_OBJ (uniq with struct destructor)
                call_cleanup_destructor(vm, record.destructor_fn_idx, ptr);
                object_free(vm, ptr);
                break;

            case 2:  // CALL_DTOR only (value struct with destructor, no heap free)
                call_cleanup_destructor(vm, record.destructor_fn_idx, ptr);
                break;

            case 3:  // LIST_CLEANUP (noncopyable list)
            case 4:  // MAP_CLEANUP (noncopyable map)
                // Basic cleanup: free the container. Element-level cleanup for
                // noncopyable elements is not yet supported during exception unwinding.
                object_free(vm, ptr);
                break;
        }

        // Null-ify the register to prevent double-cleanup
        regs[record.register_idx] = 0;
    }
}

// Computed goto (threaded dispatch) for GCC/Clang.
// Falls back to switch-based dispatch on other compilers (e.g. MSVC).
#if defined(__GNUC__) || defined(__clang__)
#define RX_USE_COMPUTED_GOTO 1
#else
#define RX_USE_COMPUTED_GOTO 0
#endif

#if RX_USE_COMPUTED_GOTO
#define OP(name) op_##name:
#define DISPATCH() do {          \
    instr = *pc++;               \
    goto *dispatch_table[instr >> 24]; \
} while(0)
#else
#define OP(name) case Opcode::name:
#define DISPATCH() break
#endif

bool interpret(RoxyVM* vm, u32 stop_depth) {
    if (vm->call_stack_empty()) {
        vm->error = "No call frame";
        return false;
    }

    // Cache current frame
    CallFrame* frame = &vm->call_stack_back();
    const BCFunction* func = frame->func;
    const u32* pc = frame->pc;
    u64* regs = frame->registers;

    u32 instr;

#if RX_USE_COMPUTED_GOTO
    // 256-entry dispatch table, one per possible opcode byte value.
    // Unused entries point to op_DEFAULT (unknown opcode error handler).
    static void* dispatch_table[256] = {
        // 0x00-0x0F: Constants and Moves
        [0x00] = &&op_LOAD_NULL,
        [0x01] = &&op_LOAD_TRUE,
        [0x02] = &&op_LOAD_FALSE,
        [0x03] = &&op_LOAD_INT,
        [0x04] = &&op_LOAD_CONST,
        [0x05] = &&op_MOV,
        [0x06] = &&op_DEFAULT, [0x07] = &&op_DEFAULT,
        [0x08] = &&op_DEFAULT, [0x09] = &&op_DEFAULT,
        [0x0A] = &&op_DEFAULT, [0x0B] = &&op_DEFAULT,
        [0x0C] = &&op_DEFAULT, [0x0D] = &&op_DEFAULT,
        [0x0E] = &&op_DEFAULT, [0x0F] = &&op_DEFAULT,

        // 0x10-0x1F: Integer Arithmetic
        [0x10] = &&op_ADD_I,
        [0x11] = &&op_SUB_I,
        [0x12] = &&op_MUL_I,
        [0x13] = &&op_DIV_I,
        [0x14] = &&op_MOD_I,
        [0x15] = &&op_NEG_I,
        [0x16] = &&op_DEFAULT, [0x17] = &&op_DEFAULT,
        [0x18] = &&op_DEFAULT, [0x19] = &&op_DEFAULT,
        [0x1A] = &&op_DEFAULT, [0x1B] = &&op_DEFAULT,
        [0x1C] = &&op_DEFAULT, [0x1D] = &&op_DEFAULT,
        [0x1E] = &&op_DEFAULT, [0x1F] = &&op_DEFAULT,

        // 0x20-0x2F: Float Arithmetic
        [0x20] = &&op_ADD_F,
        [0x21] = &&op_SUB_F,
        [0x22] = &&op_MUL_F,
        [0x23] = &&op_DIV_F,
        [0x24] = &&op_NEG_F,
        [0x25] = &&op_ADD_D,
        [0x26] = &&op_SUB_D,
        [0x27] = &&op_MUL_D,
        [0x28] = &&op_DIV_D,
        [0x29] = &&op_NEG_D,
        [0x2A] = &&op_DEFAULT, [0x2B] = &&op_DEFAULT,
        [0x2C] = &&op_DEFAULT, [0x2D] = &&op_DEFAULT,
        [0x2E] = &&op_DEFAULT, [0x2F] = &&op_DEFAULT,

        // 0x30-0x3F: Bitwise Operations
        [0x30] = &&op_BIT_AND,
        [0x31] = &&op_BIT_OR,
        [0x32] = &&op_BIT_XOR,
        [0x33] = &&op_BIT_NOT,
        [0x34] = &&op_SHL,
        [0x35] = &&op_SHR,
        [0x36] = &&op_USHR,
        [0x37] = &&op_DEFAULT, [0x38] = &&op_DEFAULT,
        [0x39] = &&op_DEFAULT, [0x3A] = &&op_DEFAULT,
        [0x3B] = &&op_DEFAULT, [0x3C] = &&op_DEFAULT,
        [0x3D] = &&op_DEFAULT, [0x3E] = &&op_DEFAULT,
        [0x3F] = &&op_DEFAULT,

        // 0x40-0x4F: Integer Comparisons
        [0x40] = &&op_EQ_I,
        [0x41] = &&op_NE_I,
        [0x42] = &&op_LT_I,
        [0x43] = &&op_LE_I,
        [0x44] = &&op_GT_I,
        [0x45] = &&op_GE_I,
        [0x46] = &&op_LT_U,
        [0x47] = &&op_LE_U,
        [0x48] = &&op_GT_U,
        [0x49] = &&op_GE_U,
        [0x4A] = &&op_DEFAULT, [0x4B] = &&op_DEFAULT,
        [0x4C] = &&op_DEFAULT, [0x4D] = &&op_DEFAULT,
        [0x4E] = &&op_DEFAULT, [0x4F] = &&op_DEFAULT,

        // 0x50-0x5F: Float Comparisons
        [0x50] = &&op_EQ_F,
        [0x51] = &&op_NE_F,
        [0x52] = &&op_LT_F,
        [0x53] = &&op_LE_F,
        [0x54] = &&op_GT_F,
        [0x55] = &&op_GE_F,
        [0x56] = &&op_EQ_D,
        [0x57] = &&op_NE_D,
        [0x58] = &&op_LT_D,
        [0x59] = &&op_LE_D,
        [0x5A] = &&op_GT_D,
        [0x5B] = &&op_GE_D,
        [0x5C] = &&op_DEFAULT, [0x5D] = &&op_DEFAULT,
        [0x5E] = &&op_DEFAULT, [0x5F] = &&op_DEFAULT,

        // 0x60-0x6F: Logical Operations
        [0x60] = &&op_NOT,
        [0x61] = &&op_AND,
        [0x62] = &&op_OR,
        [0x63] = &&op_DEFAULT, [0x64] = &&op_DEFAULT,
        [0x65] = &&op_DEFAULT, [0x66] = &&op_DEFAULT,
        [0x67] = &&op_DEFAULT, [0x68] = &&op_DEFAULT,
        [0x69] = &&op_DEFAULT, [0x6A] = &&op_DEFAULT,
        [0x6B] = &&op_DEFAULT, [0x6C] = &&op_DEFAULT,
        [0x6D] = &&op_DEFAULT, [0x6E] = &&op_DEFAULT,
        [0x6F] = &&op_DEFAULT,

        // 0x70-0x7F: Unused
        [0x70] = &&op_DEFAULT, [0x71] = &&op_DEFAULT,
        [0x72] = &&op_DEFAULT, [0x73] = &&op_DEFAULT,
        [0x74] = &&op_DEFAULT, [0x75] = &&op_DEFAULT,
        [0x76] = &&op_DEFAULT, [0x77] = &&op_DEFAULT,
        [0x78] = &&op_DEFAULT, [0x79] = &&op_DEFAULT,
        [0x7A] = &&op_DEFAULT, [0x7B] = &&op_DEFAULT,
        [0x7C] = &&op_DEFAULT, [0x7D] = &&op_DEFAULT,
        [0x7E] = &&op_DEFAULT, [0x7F] = &&op_DEFAULT,

        // 0x80-0x8F: Type Conversions
        [0x80] = &&op_I_TO_F64,
        [0x81] = &&op_F64_TO_I,
        [0x82] = &&op_I_TO_B,
        [0x83] = &&op_B_TO_I,
        [0x84] = &&op_TRUNC_S,
        [0x85] = &&op_TRUNC_U,
        [0x86] = &&op_F32_TO_F64,
        [0x87] = &&op_F64_TO_F32,
        [0x88] = &&op_I_TO_F32,
        [0x89] = &&op_F32_TO_I,
        [0x8A] = &&op_DEFAULT, [0x8B] = &&op_DEFAULT,
        [0x8C] = &&op_DEFAULT, [0x8D] = &&op_DEFAULT,
        [0x8E] = &&op_DEFAULT, [0x8F] = &&op_DEFAULT,

        // 0x90-0x9F: Control Flow
        [0x90] = &&op_JMP,
        [0x91] = &&op_JMP_IF,
        [0x92] = &&op_JMP_IF_NOT,
        [0x93] = &&op_RET,
        [0x94] = &&op_RET_VOID,
        [0x95] = &&op_JMP_IF_LT_I,
        [0x96] = &&op_JMP_IF_LE_I,
        [0x97] = &&op_JMP_IF_GT_I,
        [0x98] = &&op_JMP_IF_GE_I,
        [0x99] = &&op_JMP_IF_EQ_I,
        [0x9A] = &&op_JMP_IF_NE_I,
        [0x9B] = &&op_DEFAULT, [0x9C] = &&op_DEFAULT,
        [0x9D] = &&op_DEFAULT, [0x9E] = &&op_DEFAULT,
        [0x9F] = &&op_DEFAULT,

        // 0xA0-0xAF: Function Calls and Container Indexing
        [0xA0] = &&op_CALL,
        [0xA1] = &&op_CALL_NATIVE,
        [0xA2] = &&op_INDEX_GET_LIST,
        [0xA3] = &&op_INDEX_SET_LIST,
        [0xA4] = &&op_INDEX_GET_MAP,
        [0xA5] = &&op_INDEX_SET_MAP,
        [0xA6] = &&op_DEFAULT, [0xA7] = &&op_DEFAULT,
        [0xA8] = &&op_DEFAULT, [0xA9] = &&op_DEFAULT,
        [0xAA] = &&op_DEFAULT, [0xAB] = &&op_DEFAULT,
        [0xAC] = &&op_DEFAULT, [0xAD] = &&op_DEFAULT,
        [0xAE] = &&op_DEFAULT, [0xAF] = &&op_DEFAULT,

        // 0xB0-0xBF: Field and Stack Access
        [0xB0] = &&op_GET_FIELD,
        [0xB1] = &&op_SET_FIELD,
        [0xB2] = &&op_STACK_ADDR,
        [0xB3] = &&op_GET_FIELD_ADDR,
        [0xB4] = &&op_STRUCT_LOAD_REGS,
        [0xB5] = &&op_STRUCT_STORE_REGS,
        [0xB6] = &&op_STRUCT_COPY,
        [0xB7] = &&op_RET_STRUCT_SMALL,
        [0xB8] = &&op_SPILL_REG,
        [0xB9] = &&op_RELOAD_REG,
        [0xBA] = &&op_DEFAULT, [0xBB] = &&op_DEFAULT,
        [0xBC] = &&op_DEFAULT, [0xBD] = &&op_DEFAULT,
        [0xBE] = &&op_DEFAULT, [0xBF] = &&op_DEFAULT,

        // 0xC0-0xCF: Unused
        [0xC0] = &&op_DEFAULT, [0xC1] = &&op_DEFAULT,
        [0xC2] = &&op_DEFAULT, [0xC3] = &&op_DEFAULT,
        [0xC4] = &&op_DEFAULT, [0xC5] = &&op_DEFAULT,
        [0xC6] = &&op_DEFAULT, [0xC7] = &&op_DEFAULT,
        [0xC8] = &&op_DEFAULT, [0xC9] = &&op_DEFAULT,
        [0xCA] = &&op_DEFAULT, [0xCB] = &&op_DEFAULT,
        [0xCC] = &&op_DEFAULT, [0xCD] = &&op_DEFAULT,
        [0xCE] = &&op_DEFAULT, [0xCF] = &&op_DEFAULT,

        // 0xD0-0xDF: Object Lifecycle and Exceptions
        [0xD0] = &&op_NEW_OBJ,
        [0xD1] = &&op_DEL_OBJ,
        [0xD2] = &&op_THROW,
        [0xD3] = &&op_CALL_EXC_MSG,
        [0xD4] = &&op_DEFAULT, [0xD5] = &&op_DEFAULT,
        [0xD6] = &&op_DEFAULT, [0xD7] = &&op_DEFAULT,
        [0xD8] = &&op_DEFAULT, [0xD9] = &&op_DEFAULT,
        [0xDA] = &&op_DEFAULT, [0xDB] = &&op_DEFAULT,
        [0xDC] = &&op_DEFAULT, [0xDD] = &&op_DEFAULT,
        [0xDE] = &&op_DEFAULT, [0xDF] = &&op_DEFAULT,

        // 0xE0-0xEF: Reference Counting
        [0xE0] = &&op_REF_INC,
        [0xE1] = &&op_REF_DEC,
        [0xE2] = &&op_WEAK_CHECK,
        [0xE3] = &&op_WEAK_CREATE,
        [0xE4] = &&op_DEFAULT, [0xE5] = &&op_DEFAULT,
        [0xE6] = &&op_DEFAULT, [0xE7] = &&op_DEFAULT,
        [0xE8] = &&op_DEFAULT, [0xE9] = &&op_DEFAULT,
        [0xEA] = &&op_DEFAULT, [0xEB] = &&op_DEFAULT,
        [0xEC] = &&op_DEFAULT, [0xED] = &&op_DEFAULT,
        [0xEE] = &&op_DEFAULT, [0xEF] = &&op_DEFAULT,

        // 0xF0-0xFF: Debug/Error
        [0xF0] = &&op_TRAP,
        [0xF1] = &&op_DEFAULT, [0xF2] = &&op_DEFAULT,
        [0xF3] = &&op_DEFAULT, [0xF4] = &&op_DEFAULT,
        [0xF5] = &&op_DEFAULT, [0xF6] = &&op_DEFAULT,
        [0xF7] = &&op_DEFAULT, [0xF8] = &&op_DEFAULT,
        [0xF9] = &&op_DEFAULT, [0xFA] = &&op_DEFAULT,
        [0xFB] = &&op_DEFAULT, [0xFC] = &&op_DEFAULT,
        [0xFD] = &&op_DEFAULT,
        [0xFE] = &&op_NOP,
        [0xFF] = &&op_HALT,
    };

    // Initial dispatch
    DISPATCH();
#else
    // Main dispatch loop (switch-based fallback)
    for (;;) {
        instr = *pc++;
        switch (decode_opcode(instr)) {
#endif

    // ── Constants and Moves ──

    OP(LOAD_NULL) {
        regs[decode_a(instr)] = 0;
        DISPATCH();
    }

    OP(LOAD_TRUE) {
        regs[decode_a(instr)] = 1;
        DISPATCH();
    }

    OP(LOAD_FALSE) {
        regs[decode_a(instr)] = 0;
        DISPATCH();
    }

    OP(LOAD_INT) {
        regs[decode_a(instr)] = reg_from_i64(static_cast<i16>(decode_imm16(instr)));
        DISPATCH();
    }

    OP(LOAD_CONST) {
        regs[decode_a(instr)] = load_constant(vm, func, decode_imm16(instr));
        DISPATCH();
    }

    OP(MOV) {
        regs[decode_a(instr)] = regs[decode_b(instr)];
        DISPATCH();
    }

    // ── Integer Arithmetic ──

    OP(ADD_I) {
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) + reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(SUB_I) {
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) - reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(MUL_I) {
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) * reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(DIV_I) {
        i64 divisor = reg_as_i64(regs[decode_c(instr)]);
        if (divisor == 0) {
            vm->error = "Division by zero";
            return false;
        }
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) / divisor);
        DISPATCH();
    }

    OP(MOD_I) {
        i64 divisor = reg_as_i64(regs[decode_c(instr)]);
        if (divisor == 0) {
            vm->error = "Division by zero";
            return false;
        }
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) % divisor);
        DISPATCH();
    }

    OP(NEG_I) {
        regs[decode_a(instr)] = reg_from_i64(-reg_as_i64(regs[decode_b(instr)]));
        DISPATCH();
    }

    // ── f32 Arithmetic ──

    OP(ADD_F) {
        regs[decode_a(instr)] = reg_from_f32(reg_as_f32(regs[decode_b(instr)]) + reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(SUB_F) {
        regs[decode_a(instr)] = reg_from_f32(reg_as_f32(regs[decode_b(instr)]) - reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(MUL_F) {
        regs[decode_a(instr)] = reg_from_f32(reg_as_f32(regs[decode_b(instr)]) * reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(DIV_F) {
        regs[decode_a(instr)] = reg_from_f32(reg_as_f32(regs[decode_b(instr)]) / reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(NEG_F) {
        regs[decode_a(instr)] = reg_from_f32(-reg_as_f32(regs[decode_b(instr)]));
        DISPATCH();
    }

    // ── f64 Arithmetic ──

    OP(ADD_D) {
        regs[decode_a(instr)] = reg_from_f64(reg_as_f64(regs[decode_b(instr)]) + reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(SUB_D) {
        regs[decode_a(instr)] = reg_from_f64(reg_as_f64(regs[decode_b(instr)]) - reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(MUL_D) {
        regs[decode_a(instr)] = reg_from_f64(reg_as_f64(regs[decode_b(instr)]) * reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(DIV_D) {
        regs[decode_a(instr)] = reg_from_f64(reg_as_f64(regs[decode_b(instr)]) / reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(NEG_D) {
        regs[decode_a(instr)] = reg_from_f64(-reg_as_f64(regs[decode_b(instr)]));
        DISPATCH();
    }

    // ── Bitwise Operations ──

    OP(BIT_AND) {
        regs[decode_a(instr)] = regs[decode_b(instr)] & regs[decode_c(instr)];
        DISPATCH();
    }

    OP(BIT_OR) {
        regs[decode_a(instr)] = regs[decode_b(instr)] | regs[decode_c(instr)];
        DISPATCH();
    }

    OP(BIT_XOR) {
        regs[decode_a(instr)] = regs[decode_b(instr)] ^ regs[decode_c(instr)];
        DISPATCH();
    }

    OP(BIT_NOT) {
        regs[decode_a(instr)] = ~regs[decode_b(instr)];
        DISPATCH();
    }

    OP(SHL) {
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) << regs[decode_c(instr)]);
        DISPATCH();
    }

    OP(SHR) {
        regs[decode_a(instr)] = reg_from_i64(reg_as_i64(regs[decode_b(instr)]) >> regs[decode_c(instr)]);
        DISPATCH();
    }

    OP(USHR) {
        regs[decode_a(instr)] = regs[decode_b(instr)] >> regs[decode_c(instr)];
        DISPATCH();
    }

    // ── Integer Comparisons ──

    OP(EQ_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) == reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(NE_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) != reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LT_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) < reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LE_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) <= reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GT_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) > reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GE_I) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) >= reg_as_i64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LT_U) {
        regs[decode_a(instr)] = reg_from_bool(regs[decode_b(instr)] < regs[decode_c(instr)]);
        DISPATCH();
    }

    OP(LE_U) {
        regs[decode_a(instr)] = reg_from_bool(regs[decode_b(instr)] <= regs[decode_c(instr)]);
        DISPATCH();
    }

    OP(GT_U) {
        regs[decode_a(instr)] = reg_from_bool(regs[decode_b(instr)] > regs[decode_c(instr)]);
        DISPATCH();
    }

    OP(GE_U) {
        regs[decode_a(instr)] = reg_from_bool(regs[decode_b(instr)] >= regs[decode_c(instr)]);
        DISPATCH();
    }

    // ── f32 Comparisons ──

    OP(EQ_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) == reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(NE_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) != reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LT_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) < reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LE_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) <= reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GT_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) > reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GE_F) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f32(regs[decode_b(instr)]) >= reg_as_f32(regs[decode_c(instr)]));
        DISPATCH();
    }

    // ── f64 Comparisons ──

    OP(EQ_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) == reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(NE_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) != reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LT_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) < reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(LE_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) <= reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GT_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) > reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(GE_D) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_f64(regs[decode_b(instr)]) >= reg_as_f64(regs[decode_c(instr)]));
        DISPATCH();
    }

    // ── Logical Operations ──

    OP(NOT) {
        regs[decode_a(instr)] = reg_from_bool(!reg_is_truthy(regs[decode_b(instr)]));
        DISPATCH();
    }

    OP(AND) {
        regs[decode_a(instr)] = reg_from_bool(reg_is_truthy(regs[decode_b(instr)]) && reg_is_truthy(regs[decode_c(instr)]));
        DISPATCH();
    }

    OP(OR) {
        regs[decode_a(instr)] = reg_from_bool(reg_is_truthy(regs[decode_b(instr)]) || reg_is_truthy(regs[decode_c(instr)]));
        DISPATCH();
    }

    // ── Type Conversions ──

    OP(I_TO_F64) {
        regs[decode_a(instr)] = reg_from_f64(static_cast<f64>(reg_as_i64(regs[decode_b(instr)])));
        DISPATCH();
    }

    OP(F64_TO_I) {
        regs[decode_a(instr)] = reg_from_i64(static_cast<i64>(reg_as_f64(regs[decode_b(instr)])));
        DISPATCH();
    }

    OP(I_TO_B) {
        regs[decode_a(instr)] = reg_from_bool(reg_as_i64(regs[decode_b(instr)]) != 0);
        DISPATCH();
    }

    OP(B_TO_I) {
        regs[decode_a(instr)] = reg_is_truthy(regs[decode_b(instr)]) ? 1 : 0;
        DISPATCH();
    }

    OP(TRUNC_S) {
        u8 a = decode_a(instr);
        u8 bits = decode_c(instr);
        i64 val = reg_as_i64(regs[decode_b(instr)]);
        switch (bits) {
            case 8:  regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i8>(val))); break;
            case 16: regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i16>(val))); break;
            case 32: regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i32>(val))); break;
            default: regs[a] = regs[decode_b(instr)]; break;
        }
        DISPATCH();
    }

    OP(TRUNC_U) {
        u8 a = decode_a(instr);
        u8 bits = decode_c(instr);
        u64 val = regs[decode_b(instr)];
        switch (bits) {
            case 8:  regs[a] = val & 0xFF; break;
            case 16: regs[a] = val & 0xFFFF; break;
            case 32: regs[a] = val & 0xFFFFFFFF; break;
            default: regs[a] = val; break;
        }
        DISPATCH();
    }

    OP(F32_TO_F64) {
        f32 fval;
        u32 bits32 = static_cast<u32>(regs[decode_b(instr)]);
        memcpy(&fval, &bits32, sizeof(fval));
        regs[decode_a(instr)] = reg_from_f64(static_cast<f64>(fval));
        DISPATCH();
    }

    OP(F64_TO_F32) {
        f64 fval = reg_as_f64(regs[decode_b(instr)]);
        f32 result = static_cast<f32>(fval);
        u32 bits32;
        memcpy(&bits32, &result, sizeof(bits32));
        regs[decode_a(instr)] = static_cast<u64>(bits32);
        DISPATCH();
    }

    OP(I_TO_F32) {
        i64 ival = reg_as_i64(regs[decode_b(instr)]);
        f32 result = static_cast<f32>(ival);
        u32 bits32;
        memcpy(&bits32, &result, sizeof(bits32));
        regs[decode_a(instr)] = static_cast<u64>(bits32);
        DISPATCH();
    }

    OP(F32_TO_I) {
        f32 fval;
        u32 bits32 = static_cast<u32>(regs[decode_b(instr)]);
        memcpy(&fval, &bits32, sizeof(fval));
        regs[decode_a(instr)] = static_cast<u64>(static_cast<i64>(fval));
        DISPATCH();
    }

    // ── Control Flow ──

    OP(JMP) {
        pc += decode_offset(instr);
        DISPATCH();
    }

    OP(JMP_IF) {
        if (reg_is_truthy(regs[decode_a(instr)])) {
            pc += decode_offset(instr);
        }
        DISPATCH();
    }

    OP(JMP_IF_NOT) {
        if (!reg_is_truthy(regs[decode_a(instr)])) {
            pc += decode_offset(instr);
        }
        DISPATCH();
    }

    // ── Fused Compare-and-Branch (two-word: ABC + offset) ──

    OP(JMP_IF_LT_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) < reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(JMP_IF_LE_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) <= reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(JMP_IF_GT_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) > reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(JMP_IF_GE_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) >= reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(JMP_IF_EQ_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) == reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(JMP_IF_NE_I) {
        i32 offset = static_cast<i32>(*pc++);
        if (reg_as_i64(regs[decode_b(instr)]) != reg_as_i64(regs[decode_c(instr)])) {
            pc += offset;
        }
        DISPATCH();
    }

    OP(RET) {
        u64 result = regs[decode_a(instr)];

        u32 return_reg = frame->return_reg;
        u32 local_stack_base = frame->local_stack_base;

        --vm->call_stack_size;
        vm->register_top -= func->register_count;
        vm->local_stack_top = local_stack_base;

        if (vm->call_stack_empty()) {
            vm->register_file[0] = result;
            return true;
        }

        if (stop_depth > 0 && vm->call_stack_size <= stop_depth) {
            return true;
        }

        frame = &vm->call_stack_back();
        func = frame->func;
        pc = frame->pc;
        regs = frame->registers;

        regs[return_reg] = result;
        DISPATCH();
    }

    OP(RET_VOID) {
        u32 local_stack_base = frame->local_stack_base;

        --vm->call_stack_size;
        vm->register_top -= func->register_count;
        vm->local_stack_top = local_stack_base;

        if (vm->call_stack_empty()) {
            vm->register_file[0] = 0;
            return true;
        }

        if (stop_depth > 0 && vm->call_stack_size <= stop_depth) {
            return true;
        }

        frame = &vm->call_stack_back();
        func = frame->func;
        pc = frame->pc;
        regs = frame->registers;
        DISPATCH();
    }

    // ── Function Calls ──

    OP(CALL) {
        u8 dst = decode_a(instr);
        u8 func_idx = decode_b(instr);
        u8 arg_count = decode_c(instr);

        assert(func_idx < vm->function_count);
        const BCFunction* callee = vm->function_ptrs[func_idx];
        u8 first_arg = dst + callee->ret_reg_count;

        assert(arg_count == callee->param_count);

        if (vm->register_top + callee->register_count > vm->register_file_size) {
            vm->error = "Register file overflow";
            return false;
        }

        frame->pc = pc;

        u64* callee_regs = &vm->register_file[vm->register_top];
        vm->register_top += callee->register_count;

        // Copy arguments first (memcpy is safe since src/dst don't overlap)
        memcpy(callee_regs, &regs[first_arg], callee->param_register_count * sizeof(u64));

        // Zero remaining registers (debug only — SSA guarantees write-before-read)
#ifndef NDEBUG
        for (u32 i = callee->param_register_count; i < callee->register_count; i++) {
            callee_regs[i] = 0;
        }
#endif

        u32 local_stack_base = (vm->local_stack_top + 3) & ~3u;
        if (local_stack_base + callee->local_stack_slots > vm->local_stack_size) {
            vm->error = "Local stack overflow";
            return false;
        }
        vm->local_stack_top = local_stack_base + callee->local_stack_slots;

        vm->call_stack[vm->call_stack_size++] = CallFrame(callee, callee->code.data(), callee_regs, dst, local_stack_base);

        frame = &vm->call_stack_back();
        func = frame->func;
        pc = frame->pc;
        regs = frame->registers;
        DISPATCH();
    }

    OP(CALL_NATIVE) {
        u8 dst = decode_a(instr);
        u8 func_idx = decode_b(instr);
        u8 arg_count = decode_c(instr);
        u8 first_arg = dst + 1;

        if (func_idx >= vm->module->native_functions.size()) {
            vm->error = "Invalid native function index";
            return false;
        }

        const BCNativeFunction& native = vm->module->native_functions[func_idx];

        native.func(vm, dst, arg_count, first_arg);

        if (vm->error != nullptr) {
            return false;
        }
        DISPATCH();
    }

    // ── Container Indexing ──

    OP(INDEX_GET_LIST) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        void* lst_ptr = reg_as_ptr(regs[b]);
        if (!lst_ptr) {
            vm->error = "list index: null list reference";
            return false;
        }
        u64 idx = regs[decode_c(instr)];
        ListHeader* header = get_list_header(lst_ptr);
        if (idx >= header->length) {
            vm->error = "List index out of bounds";
            return false;
        }
        if (header->element_is_inline) {
            u32* elem = header->elements + static_cast<u32>(idx) * header->element_slot_count;
            if (header->element_slot_count == 1) {
                regs[a] = static_cast<u64>(elem[0]);
            } else if (header->element_slot_count == 2) {
                regs[a] = static_cast<u64>(elem[0]) | (static_cast<u64>(elem[1]) << 32);
            } else {
                u64 val = 0;
                memcpy(&val, elem, sizeof(u32) * header->element_slot_count);
                regs[a] = val;
            }
        } else {
            regs[a] = reinterpret_cast<u64>(list_element_ptr(header, static_cast<u32>(idx)));
        }
        DISPATCH();
    }

    OP(INDEX_SET_LIST) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        void* lst_ptr = reg_as_ptr(regs[a]);
        if (!lst_ptr) {
            vm->error = "list index_mut: null list reference";
            return false;
        }
        u64 idx = regs[b];
        ListHeader* header = get_list_header(lst_ptr);
        if (idx >= header->length) {
            vm->error = "List index out of bounds";
            return false;
        }
        if (header->element_is_inline) {
            u8 c = decode_c(instr);
            u32* elem = header->elements + static_cast<u32>(idx) * header->element_slot_count;
            if (header->element_slot_count == 1) {
                elem[0] = static_cast<u32>(regs[c]);
            } else if (header->element_slot_count == 2) {
                elem[0] = static_cast<u32>(regs[c]);
                elem[1] = static_cast<u32>(regs[c] >> 32);
            } else {
                memcpy(elem, &regs[c], sizeof(u32) * header->element_slot_count);
            }
        } else {
            u32* src = reinterpret_cast<u32*>(regs[decode_c(instr)]);
            memcpy(list_element_ptr(header, static_cast<u32>(idx)),
                   src, sizeof(u32) * header->element_slot_count);
        }
        DISPATCH();
    }

    OP(INDEX_GET_MAP) {
        u8 a = decode_a(instr);
        void* map_ptr = reg_as_ptr(regs[decode_b(instr)]);
        if (!map_ptr) {
            vm->error = "map index: null map reference";
            return false;
        }
        u64 value;
        if (!map_get(map_ptr, regs[decode_c(instr)], value, &vm->error)) {
            return false;
        }
        regs[a] = value;
        DISPATCH();
    }

    OP(INDEX_SET_MAP) {
        void* map_ptr = reg_as_ptr(regs[decode_a(instr)]);
        if (!map_ptr) {
            vm->error = "map index_mut: null map reference";
            return false;
        }
        map_insert(map_ptr, regs[decode_b(instr)], regs[decode_c(instr)]);
        DISPATCH();
    }

    // ── Stack Address ──

    OP(STACK_ADDR) {
        u16 slot_offset = decode_imm16(instr);
        u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
        regs[decode_a(instr)] = reg_from_ptr(addr);
        DISPATCH();
    }

    // ── Field Access ──

    OP(GET_FIELD) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        u8 slot_count = decode_c(instr);
        u16 slot_offset = static_cast<u16>(*pc++);

        u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[b]));
        u32* field = base + slot_offset;

        if (slot_count == 1) {
            regs[a] = static_cast<u64>(*field);
        } else if (slot_count == 2) {
            regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
        } else {
            regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
            regs[a + 1] = (slot_count >= 4)
                ? (static_cast<u64>(field[2]) | (static_cast<u64>(field[3]) << 32))
                : static_cast<u64>(field[2]);
        }
        DISPATCH();
    }

    OP(GET_FIELD_ADDR) {
        u16 slot_offset = static_cast<u16>(*pc++);
        u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[decode_b(instr)]));
        u32* field_addr = base + slot_offset;
        regs[decode_a(instr)] = reg_from_ptr(field_addr);
        DISPATCH();
    }

    OP(SET_FIELD) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        u8 slot_count = decode_c(instr);
        u16 slot_offset = static_cast<u16>(*pc++);

        u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[a]));
        u32* field = base + slot_offset;
        u64 val = regs[b];

        if (slot_count == 1) {
            *field = static_cast<u32>(val);
        } else if (slot_count == 2) {
            field[0] = static_cast<u32>(val);
            field[1] = static_cast<u32>(val >> 32);
        } else {
            field[0] = static_cast<u32>(val);
            field[1] = static_cast<u32>(val >> 32);
            u64 val2 = regs[b + 1];
            field[2] = static_cast<u32>(val2);
            if (slot_count >= 4) field[3] = static_cast<u32>(val2 >> 32);
        }
        DISPATCH();
    }

    OP(STRUCT_LOAD_REGS) {
        u8 dst_reg = decode_a(instr);
        u8 src_ptr_reg = decode_b(instr);
        u8 slot_count = decode_c(instr);
        pc++;  // Skip padding word

        u32* src = reinterpret_cast<u32*>(regs[src_ptr_reg]);
        u8 reg_count = (slot_count + 1) / 2;

        for (u8 r = 0; r < reg_count; r++) {
            u32 slot_idx = r * 2;
            u64 value = 0;
            if (slot_idx < slot_count) value = src[slot_idx];
            if (slot_idx + 1 < slot_count) value |= static_cast<u64>(src[slot_idx + 1]) << 32;
            regs[dst_reg + r] = value;
        }
        DISPATCH();
    }

    OP(STRUCT_STORE_REGS) {
        u8 dst_ptr_reg = decode_a(instr);
        u8 src_reg = decode_b(instr);
        u8 slot_count = decode_c(instr);
        pc++;  // Skip padding word

        u32* dst = reinterpret_cast<u32*>(regs[dst_ptr_reg]);
        u8 reg_count = (slot_count + 1) / 2;

        for (u8 r = 0; r < reg_count; r++) {
            u64 value = regs[src_reg + r];
            u32 slot_idx = r * 2;
            if (slot_idx < slot_count) dst[slot_idx] = static_cast<u32>(value);
            if (slot_idx + 1 < slot_count) dst[slot_idx + 1] = static_cast<u32>(value >> 32);
        }
        DISPATCH();
    }

    OP(STRUCT_COPY) {
        u8 dst_ptr_reg = decode_a(instr);
        u8 src_ptr_reg = decode_b(instr);
        u8 slot_count = decode_c(instr);
        u32* dst = reinterpret_cast<u32*>(regs[dst_ptr_reg]);
        u32* src = reinterpret_cast<u32*>(regs[src_ptr_reg]);
        for (u8 i = 0; i < slot_count; i++) {
            dst[i] = src[i];
        }
        DISPATCH();
    }

    OP(RET_STRUCT_SMALL) {
        u8 src_ptr_reg = decode_a(instr);
        u8 slot_count = decode_b(instr);
        u32* src = reinterpret_cast<u32*>(regs[src_ptr_reg]);
        u8 reg_count = (slot_count + 1) / 2;

        u64 ret_vals[2] = {0, 0};
        for (u8 r = 0; r < reg_count; r++) {
            u32 slot_idx = r * 2;
            if (slot_idx < slot_count) ret_vals[r] = src[slot_idx];
            if (slot_idx + 1 < slot_count) ret_vals[r] |= static_cast<u64>(src[slot_idx + 1]) << 32;
        }

        u8 return_reg = frame->return_reg;
        u32 local_stack_base = frame->local_stack_base;

        --vm->call_stack_size;
        vm->register_top -= func->register_count;
        vm->local_stack_top = local_stack_base;

        if (vm->call_stack_empty()) {
            for (u8 r = 0; r < reg_count; r++) {
                vm->register_file[r] = ret_vals[r];
            }
            return true;
        }

        if (stop_depth > 0 && vm->call_stack_size <= stop_depth) {
            return true;
        }

        frame = &vm->call_stack_back();
        func = frame->func;
        pc = frame->pc;
        regs = frame->registers;

        for (u8 r = 0; r < reg_count; r++) {
            regs[return_reg + r] = ret_vals[r];
        }
        DISPATCH();
    }

    // ── Spill/Reload ──

    OP(SPILL_REG) {
        u8 a = decode_a(instr);
        u16 slot_offset = decode_imm16(instr);
        u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
        u64 val = regs[a];
        addr[0] = static_cast<u32>(val);
        addr[1] = static_cast<u32>(val >> 32);
        DISPATCH();
    }

    OP(RELOAD_REG) {
        u16 slot_offset = decode_imm16(instr);
        u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
        regs[decode_a(instr)] = static_cast<u64>(addr[0]) | (static_cast<u64>(addr[1]) << 32);
        DISPATCH();
    }

    // ── Object Lifecycle ──

    OP(NEW_OBJ) {
        u16 type_idx = decode_imm16(instr);
        if (type_idx >= vm->module->type_ids.size()) {
            vm->error = "Invalid type index";
            return false;
        }
        u32 type_id = vm->module->type_ids[type_idx];

        const ObjectTypeInfo* type_info = get_object_type(type_id);
        if (type_info == nullptr) {
            vm->error = "Invalid type ID";
            return false;
        }

        void* data = object_alloc(vm, type_id, type_info->size);
        if (data == nullptr) {
            vm->error = "Memory allocation failed";
            return false;
        }

        regs[decode_a(instr)] = reg_from_ptr(data);
        DISPATCH();
    }

    OP(DEL_OBJ) {
        u8 a = decode_a(instr);
        void* ptr = reg_as_ptr(regs[a]);
        if (ptr != nullptr) {
            ObjectHeader* header = get_header_from_data(ptr);
            if (header->ref_count > 0) {
                vm->error = "Cannot delete: object has active borrows";
                return false;
            }
            object_free(vm, ptr);
            regs[a] = 0;
        }
        DISPATCH();
    }

    // ── Reference Counting ──

    OP(REF_INC) {
        void* ptr = reg_as_ptr(regs[decode_a(instr)]);
        if (ptr != nullptr) {
            ref_inc(ptr);
        }
        DISPATCH();
    }

    OP(REF_DEC) {
        void* ptr = reg_as_ptr(regs[decode_a(instr)]);
        if (ptr != nullptr) {
            if (!ref_dec(vm, ptr)) return false;
        }
        DISPATCH();
    }

    OP(WEAK_CHECK) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        void* ptr = reg_as_ptr(regs[b]);
        u64 gen = regs[b + 1];

        if (ptr == nullptr) {
            regs[a] = 0;
        } else {
            bool valid = weak_ref_valid(ptr, gen);
            regs[a] = reg_from_bool(valid);
        }
        DISPATCH();
    }

    OP(WEAK_CREATE) {
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        void* ptr = reg_as_ptr(regs[b]);
        regs[a] = regs[b];
        regs[a + 1] = (ptr != nullptr) ? weak_ref_create(ptr) : 0;
        DISPATCH();
    }

    // ── Exception Handling ──

    OP(THROW) {
        void* exception_ptr = reg_as_ptr(regs[decode_a(instr)]);
        if (!exception_ptr) {
            vm->error = "throw: null exception";
            return false;
        }

        ObjectHeader* header = get_header_from_data(exception_ptr);
        u32 exception_type_id = header->type_id;

        frame->pc = pc;

        while (true) {
            u32 current_pc = static_cast<u32>(frame->pc - func->code.data());

            bool handler_found = false;
            for (const auto& handler : func->exception_handlers) {
                if (current_pc >= handler.try_start_pc && current_pc < handler.try_end_pc) {
                    bool type_matches = false;
                    if (handler.type_id == 0) {
                        type_matches = true;
                    } else {
                        u32 handler_global_type_id = vm->module->type_ids[handler.type_id - 1];
                        type_matches = (exception_type_id == handler_global_type_id);
                    }

                    if (type_matches) {
                        execute_cleanup(vm, func, current_pc, handler.handler_pc, regs);

                        frame = &vm->call_stack_back();

                        regs[handler.exception_reg] = reg_from_ptr(exception_ptr);
                        pc = func->code.data() + handler.handler_pc;
                        handler_found = true;
                        break;
                    }
                }
            }

            if (handler_found) break;

            execute_cleanup(vm, func, current_pc, UINT32_MAX, regs);

            frame = &vm->call_stack_back();

            u32 local_stack_base = frame->local_stack_base;
            --vm->call_stack_size;
            vm->register_top -= func->register_count;
            vm->local_stack_top = local_stack_base;

            if (vm->call_stack_empty()) {
                object_free(vm, exception_ptr);
                vm->error = "Unhandled exception";
                return false;
            }

            frame = &vm->call_stack_back();
            func = frame->func;
            regs = frame->registers;
        }

        DISPATCH();
    }

    OP(CALL_EXC_MSG) {
        u8 a = decode_a(instr);
        void* exception_ptr = reg_as_ptr(regs[decode_b(instr)]);
        if (exception_ptr) {
            void* msg = string_alloc(vm, "exception", 9);
            regs[a] = reg_from_ptr(msg);
        }
        DISPATCH();
    }

    // ── Debug/Error ──

    OP(NOP) {
        DISPATCH();
    }

    OP(TRAP) {
        vm->error = "Runtime error: variant field access with wrong discriminant";
        return false;
    }

    OP(HALT) {
        return true;
    }

#if RX_USE_COMPUTED_GOTO
    op_DEFAULT:
        vm->error = "Unknown opcode";
        return false;
#else
            default:
                vm->error = "Unknown opcode";
                return false;
        } // switch
    } // while

    return true;
#endif
}

#undef OP
#undef DISPATCH

}

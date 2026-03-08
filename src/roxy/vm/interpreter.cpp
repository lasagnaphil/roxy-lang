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
    if (func_idx >= vm->module->functions.size()) return;

    const BCFunction* dtor_func = vm->module->functions[func_idx].get();
    if (!dtor_func) return;

    // Check call stack depth limit
    if (vm->call_stack.size() >= 1024) return;

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
    u32 saved_depth = vm->call_stack.size();
    vm->call_stack.push_back(CallFrame(
        dtor_func, dtor_func->code.data(), dtor_regs, 0, local_stack_base));

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

bool interpret(RoxyVM* vm, u32 stop_depth) {
    if (vm->call_stack.empty()) {
        vm->error = "No call frame";
        return false;
    }

    // Cache current frame
    CallFrame* frame = &vm->call_stack.back();
    const BCFunction* func = frame->func;
    const u32* pc = frame->pc;
    u64* regs = frame->registers;

    // Main dispatch loop
    while (vm->running) {
        u32 instr = *pc++;
        Opcode op = decode_opcode(instr);
        u8 a = decode_a(instr);
        u8 b = decode_b(instr);
        u8 c = decode_c(instr);
        u16 imm = decode_imm16(instr);
        i16 offset = decode_offset(instr);

        switch (op) {
            // Constants and Moves
            case Opcode::LOAD_NULL:
                regs[a] = 0;
                break;

            case Opcode::LOAD_TRUE:
                regs[a] = 1;
                break;

            case Opcode::LOAD_FALSE:
                regs[a] = 0;
                break;

            case Opcode::LOAD_INT:
                regs[a] = reg_from_i64(static_cast<i16>(imm));
                break;

            case Opcode::LOAD_CONST:
                regs[a] = load_constant(vm, func, imm);
                break;

            case Opcode::MOV:
                regs[a] = regs[b];
                break;

            // Integer Arithmetic
            case Opcode::ADD_I:
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) + reg_as_i64(regs[c]));
                break;

            case Opcode::SUB_I:
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) - reg_as_i64(regs[c]));
                break;

            case Opcode::MUL_I:
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) * reg_as_i64(regs[c]));
                break;

            case Opcode::DIV_I: {
                i64 divisor = reg_as_i64(regs[c]);
                if (divisor == 0) {
                    vm->error = "Division by zero";
                    return false;
                }
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) / divisor);
                break;
            }

            case Opcode::MOD_I: {
                i64 divisor = reg_as_i64(regs[c]);
                if (divisor == 0) {
                    vm->error = "Division by zero";
                    return false;
                }
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) % divisor);
                break;
            }

            case Opcode::NEG_I:
                regs[a] = reg_from_i64(-reg_as_i64(regs[b]));
                break;

            // f32 Arithmetic
            case Opcode::ADD_F:
                regs[a] = reg_from_f32(reg_as_f32(regs[b]) + reg_as_f32(regs[c]));
                break;

            case Opcode::SUB_F:
                regs[a] = reg_from_f32(reg_as_f32(regs[b]) - reg_as_f32(regs[c]));
                break;

            case Opcode::MUL_F:
                regs[a] = reg_from_f32(reg_as_f32(regs[b]) * reg_as_f32(regs[c]));
                break;

            // Float/double division by zero follows IEEE 754 semantics:
            // produces +/-infinity or NaN, unlike integer division which
            // raises a runtime error above (DIV_I, MOD_I).
            case Opcode::DIV_F:
                regs[a] = reg_from_f32(reg_as_f32(regs[b]) / reg_as_f32(regs[c]));
                break;

            case Opcode::NEG_F:
                regs[a] = reg_from_f32(-reg_as_f32(regs[b]));
                break;

            // f64 Arithmetic
            case Opcode::ADD_D:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) + reg_as_f64(regs[c]));
                break;

            case Opcode::SUB_D:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) - reg_as_f64(regs[c]));
                break;

            case Opcode::MUL_D:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) * reg_as_f64(regs[c]));
                break;

            case Opcode::DIV_D:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) / reg_as_f64(regs[c]));
                break;

            case Opcode::NEG_D:
                regs[a] = reg_from_f64(-reg_as_f64(regs[b]));
                break;

            // Bitwise Operations
            case Opcode::BIT_AND:
                regs[a] = regs[b] & regs[c];
                break;

            case Opcode::BIT_OR:
                regs[a] = regs[b] | regs[c];
                break;

            case Opcode::BIT_XOR:
                regs[a] = regs[b] ^ regs[c];
                break;

            case Opcode::BIT_NOT:
                regs[a] = ~regs[b];
                break;

            case Opcode::SHL:
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) << regs[c]);
                break;

            case Opcode::SHR:
                regs[a] = reg_from_i64(reg_as_i64(regs[b]) >> regs[c]);
                break;

            case Opcode::USHR:
                regs[a] = regs[b] >> regs[c];
                break;

            // Integer Comparisons
            case Opcode::EQ_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) == reg_as_i64(regs[c]));
                break;

            case Opcode::NE_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) != reg_as_i64(regs[c]));
                break;

            case Opcode::LT_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) < reg_as_i64(regs[c]));
                break;

            case Opcode::LE_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) <= reg_as_i64(regs[c]));
                break;

            case Opcode::GT_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) > reg_as_i64(regs[c]));
                break;

            case Opcode::GE_I:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) >= reg_as_i64(regs[c]));
                break;

            case Opcode::LT_U:
                regs[a] = reg_from_bool(regs[b] < regs[c]);
                break;

            case Opcode::LE_U:
                regs[a] = reg_from_bool(regs[b] <= regs[c]);
                break;

            case Opcode::GT_U:
                regs[a] = reg_from_bool(regs[b] > regs[c]);
                break;

            case Opcode::GE_U:
                regs[a] = reg_from_bool(regs[b] >= regs[c]);
                break;

            // f32 Comparisons
            case Opcode::EQ_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) == reg_as_f32(regs[c]));
                break;

            case Opcode::NE_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) != reg_as_f32(regs[c]));
                break;

            case Opcode::LT_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) < reg_as_f32(regs[c]));
                break;

            case Opcode::LE_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) <= reg_as_f32(regs[c]));
                break;

            case Opcode::GT_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) > reg_as_f32(regs[c]));
                break;

            case Opcode::GE_F:
                regs[a] = reg_from_bool(reg_as_f32(regs[b]) >= reg_as_f32(regs[c]));
                break;

            // f64 Comparisons
            case Opcode::EQ_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) == reg_as_f64(regs[c]));
                break;

            case Opcode::NE_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) != reg_as_f64(regs[c]));
                break;

            case Opcode::LT_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) < reg_as_f64(regs[c]));
                break;

            case Opcode::LE_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) <= reg_as_f64(regs[c]));
                break;

            case Opcode::GT_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) > reg_as_f64(regs[c]));
                break;

            case Opcode::GE_D:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) >= reg_as_f64(regs[c]));
                break;

            // Logical Operations
            case Opcode::NOT:
                regs[a] = reg_from_bool(!reg_is_truthy(regs[b]));
                break;

            case Opcode::AND:
                regs[a] = reg_from_bool(reg_is_truthy(regs[b]) && reg_is_truthy(regs[c]));
                break;

            case Opcode::OR:
                regs[a] = reg_from_bool(reg_is_truthy(regs[b]) || reg_is_truthy(regs[c]));
                break;

            // Type Conversions
            case Opcode::I_TO_F64:
                regs[a] = reg_from_f64(static_cast<f64>(reg_as_i64(regs[b])));
                break;

            case Opcode::F64_TO_I:
                regs[a] = reg_from_i64(static_cast<i64>(reg_as_f64(regs[b])));
                break;

            case Opcode::I_TO_B:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) != 0);
                break;

            case Opcode::B_TO_I:
                regs[a] = reg_is_truthy(regs[b]) ? 1 : 0;
                break;

            case Opcode::TRUNC_S: {
                // Format: [TRUNC_S][dst][src][bits]
                u8 bits = c;  // 8, 16, or 32
                i64 val = reg_as_i64(regs[b]);
                switch (bits) {
                    case 8:  regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i8>(val))); break;
                    case 16: regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i16>(val))); break;
                    case 32: regs[a] = static_cast<u64>(static_cast<i64>(static_cast<i32>(val))); break;
                    default: regs[a] = regs[b]; break;
                }
                break;
            }

            case Opcode::TRUNC_U: {
                u8 bits = c;
                u64 val = regs[b];
                switch (bits) {
                    case 8:  regs[a] = val & 0xFF; break;
                    case 16: regs[a] = val & 0xFFFF; break;
                    case 32: regs[a] = val & 0xFFFFFFFF; break;
                    default: regs[a] = val; break;
                }
                break;
            }

            case Opcode::F32_TO_F64: {
                // Convert f32 (stored in lower 32 bits) to f64
                f32 fval;
                u32 bits32 = static_cast<u32>(regs[b]);
                memcpy(&fval, &bits32, sizeof(fval));
                regs[a] = reg_from_f64(static_cast<f64>(fval));
                break;
            }

            case Opcode::F64_TO_F32: {
                // Convert f64 to f32 (stored in lower 32 bits)
                f64 fval = reg_as_f64(regs[b]);
                f32 result = static_cast<f32>(fval);
                u32 bits32;
                memcpy(&bits32, &result, sizeof(bits32));
                regs[a] = static_cast<u64>(bits32);
                break;
            }

            case Opcode::I_TO_F32: {
                // Convert integer to f32 (stored in lower 32 bits)
                i64 ival = reg_as_i64(regs[b]);
                f32 result = static_cast<f32>(ival);
                u32 bits32;
                memcpy(&bits32, &result, sizeof(bits32));
                regs[a] = static_cast<u64>(bits32);
                break;
            }

            case Opcode::F32_TO_I: {
                // Convert f32 (stored in lower 32 bits) to i64
                f32 fval;
                u32 bits32 = static_cast<u32>(regs[b]);
                memcpy(&fval, &bits32, sizeof(fval));
                regs[a] = static_cast<u64>(static_cast<i64>(fval));
                break;
            }

            // Control Flow
            case Opcode::JMP:
                pc += offset;
                break;

            case Opcode::JMP_IF:
                if (reg_is_truthy(regs[a])) {
                    pc += offset;
                }
                break;

            case Opcode::JMP_IF_NOT:
                if (!reg_is_truthy(regs[a])) {
                    pc += offset;
                }
                break;

            case Opcode::RET: {
                u64 result = regs[a];

                // Save return register and local stack base before popping frame
                u32 return_reg = frame->return_reg;
                u32 local_stack_base = frame->local_stack_base;

                // Pop current frame
                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;
                vm->local_stack_top = local_stack_base;  // Deallocate local stack

                if (vm->call_stack.empty()) {
                    // Return from top-level function
                    vm->register_file[0] = result;
                    vm->running = false;
                    return true;
                }

                if (stop_depth > 0 && vm->call_stack.size() <= stop_depth) {
                    // Nested interpretation complete - don't touch register_file[0]
                    return true;
                }

                // Restore caller frame
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;

                // Store result in caller's return register (saved from callee frame)
                regs[return_reg] = result;
                break;
            }

            case Opcode::RET_VOID: {
                // Save local stack base before popping frame
                u32 local_stack_base = frame->local_stack_base;

                // Pop current frame
                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;
                vm->local_stack_top = local_stack_base;  // Deallocate local stack

                if (vm->call_stack.empty()) {
                    // Return from top-level function
                    vm->register_file[0] = 0;
                    vm->running = false;
                    return true;
                }

                if (stop_depth > 0 && vm->call_stack.size() <= stop_depth) {
                    // Nested interpretation complete - don't touch register_file[0]
                    return true;
                }

                // Restore caller frame
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;
                break;
            }

            // Function Calls
            case Opcode::CALL: {
                // Format: CALL dst, func_idx, arg_count
                // Arguments are at dst+1, dst+2, ... (set up by caller)
                u8 dst = a;
                u8 func_idx = b;
                u8 arg_count = c;
                u8 first_arg = dst + 1;  // Arguments follow the destination register

                if (func_idx >= vm->module->functions.size()) {
                    vm->error = "Invalid function index";
                    return false;
                }

                const BCFunction* callee = vm->module->functions[func_idx].get();

                // Check argument count
                if (arg_count != callee->param_count) {
                    vm->error = "Wrong number of arguments";
                    return false;
                }

                // Check register space
                if (vm->register_top + callee->register_count > vm->register_file_size) {
                    vm->error = "Register file overflow";
                    return false;
                }

                // Save current PC
                frame->pc = pc;

                // Allocate registers for callee
                u64* callee_regs = &vm->register_file[vm->register_top];
                vm->register_top += callee->register_count;

                // Clear callee registers
                for (u32 i = 0; i < callee->register_count; i++) {
                    callee_regs[i] = 0;
                }

                // Copy arguments - use param_register_count to handle multi-register struct params
                u32 reg_count = callee->param_register_count;
                for (u32 i = 0; i < reg_count; i++) {
                    callee_regs[i] = regs[first_arg + i];
                }

                // Allocate local stack space for callee (16-byte aligned)
                u32 local_stack_base = (vm->local_stack_top + 3) & ~3u;  // Align to 4 slots (16 bytes)
                if (local_stack_base + callee->local_stack_slots > vm->local_stack_size) {
                    vm->error = "Local stack overflow";
                    return false;
                }
                vm->local_stack_top = local_stack_base + callee->local_stack_slots;

                // Push new call frame
                CallFrame new_frame(callee, callee->code.data(), callee_regs, dst, local_stack_base);
                vm->call_stack.push_back(new_frame);

                // Update cached values
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;
                break;
            }

            case Opcode::CALL_NATIVE: {
                // Format: CALL_NATIVE dst, func_idx, arg_count
                // Arguments are at dst+1, dst+2, ... (set up by caller)
                u8 dst = a;
                u8 func_idx = b;
                u8 arg_count = c;
                u8 first_arg = dst + 1;

                if (func_idx >= vm->module->native_functions.size()) {
                    vm->error = "Invalid native function index";
                    return false;
                }

                const BCNativeFunction& native = vm->module->native_functions[func_idx];

                // Call native function directly
                native.func(vm, dst, arg_count, first_arg);

                if (vm->error != nullptr) {
                    return false;
                }
                break;
            }

            // Container Indexing
            case Opcode::INDEX_GET_LIST: {
                // Format: a=dst, b=obj, c=index
                void* lst_ptr = reg_as_ptr(regs[b]);
                if (!lst_ptr) {
                    vm->error = "list index: null list reference";
                    return false;
                }
                i64 idx = reg_as_i64(regs[c]);
                ListHeader* header = get_list_header(lst_ptr);
                if (idx < 0 || static_cast<u64>(idx) >= header->length) {
                    vm->error = "List index out of bounds";
                    return false;
                }
                if (header->element_is_inline) {
                    // Primitive: load 1-2 u32 slots into register as value
                    u64 val = 0;
                    memcpy(&val, list_element_ptr(header, static_cast<u32>(idx)),
                           sizeof(u32) * header->element_slot_count);
                    regs[a] = val;
                } else {
                    // Struct: return pointer to element data in buffer
                    regs[a] = reinterpret_cast<u64>(list_element_ptr(header, static_cast<u32>(idx)));
                }
                break;
            }

            case Opcode::INDEX_SET_LIST: {
                // Format: a=obj, b=index, c=value
                void* lst_ptr = reg_as_ptr(regs[a]);
                if (!lst_ptr) {
                    vm->error = "list index_mut: null list reference";
                    return false;
                }
                i64 idx = reg_as_i64(regs[b]);
                ListHeader* header = get_list_header(lst_ptr);
                if (idx < 0 || static_cast<u64>(idx) >= header->length) {
                    vm->error = "List index out of bounds";
                    return false;
                }
                if (header->element_is_inline) {
                    // Primitive: register holds the value directly
                    memcpy(list_element_ptr(header, static_cast<u32>(idx)),
                           &regs[c], sizeof(u32) * header->element_slot_count);
                } else {
                    // Struct: regs[c] is a pointer to struct data
                    u32* src = reinterpret_cast<u32*>(regs[c]);
                    memcpy(list_element_ptr(header, static_cast<u32>(idx)),
                           src, sizeof(u32) * header->element_slot_count);
                }
                break;
            }

            case Opcode::INDEX_GET_MAP: {
                // Format: a=dst, b=obj, c=key
                void* map_ptr = reg_as_ptr(regs[b]);
                if (!map_ptr) {
                    vm->error = "map index: null map reference";
                    return false;
                }
                u64 value;
                if (!map_get(map_ptr, regs[c], value, &vm->error)) {
                    return false;
                }
                regs[a] = value;
                break;
            }

            case Opcode::INDEX_SET_MAP: {
                // Format: a=obj, b=key, c=value
                void* map_ptr = reg_as_ptr(regs[a]);
                if (!map_ptr) {
                    vm->error = "map index_mut: null map reference";
                    return false;
                }
                map_insert(map_ptr, regs[b], regs[c]);
                break;
            }

            // Stack Address
            case Opcode::STACK_ADDR: {
                // Format: STACK_ADDR dst, slot_offset
                // dst = pointer to local_stack[local_stack_base + slot_offset]
                u16 slot_offset = imm;
                u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
                regs[a] = reg_from_ptr(addr);
                break;
            }

            // Field Access
            case Opcode::GET_FIELD: {
                // Format: [GET_FIELD dst obj slot_count] + [slot_offset:16 padding:16]
                u8 slot_count = c;
                u16 slot_offset = static_cast<u16>(*pc++);  // Read second instruction word

                u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[b]));
                u32* field = base + slot_offset;

                if (slot_count == 1) {
                    // 32-bit field: zero-extend to 64-bit
                    regs[a] = static_cast<u64>(*field);
                } else if (slot_count == 2) {
                    // 64-bit field: read two consecutive slots (little-endian)
                    regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
                } else {
                    // 3-4 slots: read into 2 consecutive registers (for weak refs)
                    regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
                    regs[a + 1] = (slot_count >= 4)
                        ? (static_cast<u64>(field[2]) | (static_cast<u64>(field[3]) << 32))
                        : static_cast<u64>(field[2]);
                }
                break;
            }

            case Opcode::GET_FIELD_ADDR: {
                // Format: [GET_FIELD_ADDR dst obj 0] + [slot_offset:16 padding:16]
                // Computes: dst = obj_ptr + slot_offset * sizeof(u32)
                u16 slot_offset = static_cast<u16>(*pc++);  // Read second instruction word

                u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[b]));
                u32* field_addr = base + slot_offset;

                regs[a] = reg_from_ptr(field_addr);
                break;
            }

            case Opcode::SET_FIELD: {
                // Format: [SET_FIELD obj val slot_count] + [slot_offset:16 padding:16]
                u8 slot_count = c;
                u16 slot_offset = static_cast<u16>(*pc++);  // Read second instruction word

                u32* base = reinterpret_cast<u32*>(reg_as_ptr(regs[a]));
                u32* field = base + slot_offset;
                u64 val = regs[b];

                if (slot_count == 1) {
                    // 32-bit field
                    *field = static_cast<u32>(val);
                } else if (slot_count == 2) {
                    // 64-bit field: write two consecutive slots (little-endian)
                    field[0] = static_cast<u32>(val);
                    field[1] = static_cast<u32>(val >> 32);
                } else {
                    // 3-4 slots from 2 consecutive registers (for weak refs)
                    field[0] = static_cast<u32>(val);
                    field[1] = static_cast<u32>(val >> 32);
                    u64 val2 = regs[b + 1];
                    field[2] = static_cast<u32>(val2);
                    if (slot_count >= 4) field[3] = static_cast<u32>(val2 >> 32);
                }
                break;
            }

            case Opcode::STRUCT_LOAD_REGS: {
                // Format: [STRUCT_LOAD_REGS dst src_ptr slot_count][pad]
                // Load struct data from memory to consecutive registers
                u8 dst_reg = a;
                u8 src_ptr_reg = b;
                u8 slot_count = c;
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
                break;
            }

            case Opcode::STRUCT_STORE_REGS: {
                // Format: [STRUCT_STORE_REGS dst_ptr src_reg slot_count][pad]
                // Store consecutive registers to struct memory
                u8 dst_ptr_reg = a;
                u8 src_reg = b;
                u8 slot_count = c;
                pc++;  // Skip padding word

                u32* dst = reinterpret_cast<u32*>(regs[dst_ptr_reg]);
                u8 reg_count = (slot_count + 1) / 2;

                for (u8 r = 0; r < reg_count; r++) {
                    u64 value = regs[src_reg + r];
                    u32 slot_idx = r * 2;
                    if (slot_idx < slot_count) dst[slot_idx] = static_cast<u32>(value);
                    if (slot_idx + 1 < slot_count) dst[slot_idx + 1] = static_cast<u32>(value >> 32);
                }
                break;
            }

            case Opcode::STRUCT_COPY: {
                // Format: [STRUCT_COPY dst_ptr src_ptr slot_count]
                // Memory-to-memory struct copy
                u8 dst_ptr_reg = a;
                u8 src_ptr_reg = b;
                u8 slot_count = c;
                u32* dst = reinterpret_cast<u32*>(regs[dst_ptr_reg]);
                u32* src = reinterpret_cast<u32*>(regs[src_ptr_reg]);
                for (u8 i = 0; i < slot_count; i++) {
                    dst[i] = src[i];
                }
                break;
            }

            case Opcode::RET_STRUCT_SMALL: {
                // Format: [RET_STRUCT_SMALL src_ptr slot_count 0]
                // Return small struct (≤4 slots) in registers
                u8 src_ptr_reg = a;
                u8 slot_count = b;
                u32* src = reinterpret_cast<u32*>(regs[src_ptr_reg]);
                u8 reg_count = (slot_count + 1) / 2;

                // Pack struct into temp values
                u64 ret_vals[2] = {0, 0};
                for (u8 r = 0; r < reg_count; r++) {
                    u32 slot_idx = r * 2;
                    if (slot_idx < slot_count) ret_vals[r] = src[slot_idx];
                    if (slot_idx + 1 < slot_count) ret_vals[r] |= static_cast<u64>(src[slot_idx + 1]) << 32;
                }

                // Save frame info before popping
                u8 return_reg = frame->return_reg;
                u32 local_stack_base = frame->local_stack_base;

                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;
                vm->local_stack_top = local_stack_base;

                if (vm->call_stack.empty()) {
                    // Top-level return - store in R0, R1
                    for (u8 r = 0; r < reg_count; r++) {
                        vm->register_file[r] = ret_vals[r];
                    }
                    vm->running = false;
                    return true;
                }

                if (stop_depth > 0 && vm->call_stack.size() <= stop_depth) {
                    // Nested interpretation complete - don't touch register_file
                    return true;
                }

                // Restore caller frame and store return values
                frame = &vm->call_stack.back();
                func = frame->func;
                pc = frame->pc;
                regs = frame->registers;

                for (u8 r = 0; r < reg_count; r++) {
                    regs[return_reg + r] = ret_vals[r];
                }
                break;
            }

            // Object Lifecycle
            case Opcode::NEW_OBJ: {
                // imm is the module's type index, look up the global type_id
                u16 type_idx = imm;
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

                regs[a] = reg_from_ptr(data);
                break;
            }

            case Opcode::DEL_OBJ: {
                void* ptr = reg_as_ptr(regs[a]);
                if (ptr != nullptr) {
                    // Constraint reference model: check for active borrows
                    ObjectHeader* header = get_header_from_data(ptr);
                    if (header->ref_count > 0) {
                        vm->error = "Cannot delete: object has active borrows";
                        return false;
                    }
                    object_free(vm, ptr);
                    regs[a] = 0;
                }
                break;
            }

            // Reference Counting
            case Opcode::REF_INC: {
                void* ptr = reg_as_ptr(regs[a]);
                if (ptr != nullptr) {
                    ref_inc(ptr);
                }
                break;
            }

            case Opcode::REF_DEC: {
                void* ptr = reg_as_ptr(regs[a]);
                if (ptr != nullptr) {
                    if (!ref_dec(vm, ptr)) return false;
                }
                break;
            }

            case Opcode::WEAK_CHECK: {
                // Format: WEAK_CHECK dst, ptr_reg, 0
                // Check if weak reference is still valid using 64-bit generation
                // Weak ref occupies 2 consecutive registers: ptr in regs[b], generation in regs[b+1]
                void* ptr = reg_as_ptr(regs[b]);
                u64 gen = regs[b + 1];

                if (ptr == nullptr) {
                    regs[a] = 0;  // false - null pointer
                } else {
                    // Safe to read: memory is always mapped (active or tombstoned)
                    // Tombstoned memory returns zeros, so is_alive() will be false
                    bool valid = weak_ref_valid(ptr, gen);
                    regs[a] = reg_from_bool(valid);
                }
                break;
            }

            case Opcode::WEAK_CREATE: {
                // Format: WEAK_CREATE dst, src, 0
                // Create weak ref: dst = pointer, dst+1 = generation
                void* ptr = reg_as_ptr(regs[b]);
                regs[a] = regs[b];  // Copy pointer
                regs[a + 1] = (ptr != nullptr) ? weak_ref_create(ptr) : 0;
                break;
            }

            // Spill/Reload
            case Opcode::SPILL_REG: {
                u16 slot_offset = imm;
                u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
                u64 val = regs[a];
                addr[0] = static_cast<u32>(val);
                addr[1] = static_cast<u32>(val >> 32);
                break;
            }

            case Opcode::RELOAD_REG: {
                u16 slot_offset = imm;
                u32* addr = vm->local_stack.get() + frame->local_stack_base + slot_offset;
                regs[a] = static_cast<u64>(addr[0]) | (static_cast<u64>(addr[1]) << 32);
                break;
            }

            // Debug
            case Opcode::NOP:
                break;

            case Opcode::TRAP:
                vm->error = "Runtime error: variant field access with wrong discriminant";
                return false;

            case Opcode::THROW: {
                void* exception_ptr = reg_as_ptr(regs[a]);
                if (!exception_ptr) {
                    vm->error = "throw: null exception";
                    return false;
                }

                ObjectHeader* header = get_header_from_data(exception_ptr);
                u32 exception_type_id = header->type_id;

                // Stack unwinding: search for a matching exception handler
                // Save PC for current frame before searching
                frame->pc = pc;

                while (true) {
                    // Compute current PC offset within the function
                    u32 current_pc = static_cast<u32>(frame->pc - func->code.data());

                    // Search exception handlers in current function
                    bool handler_found = false;
                    for (const auto& handler : func->exception_handlers) {
                        if (current_pc >= handler.try_start_pc && current_pc < handler.try_end_pc) {
                            // Check type match
                            bool type_matches = false;
                            if (handler.type_id == 0) {
                                // Catch-all
                                type_matches = true;
                            } else {
                                // Typed catch: compare global type_ids
                                u32 handler_global_type_id = vm->module->type_ids[handler.type_id - 1];
                                type_matches = (exception_type_id == handler_global_type_id);
                            }

                            if (type_matches) {
                                // Execute cleanup for owned locals between throw site
                                // and handler (variables in scope at throw but not at handler)
                                execute_cleanup(vm, func, current_pc, handler.handler_pc, regs);

                                // Re-cache frame pointer (execute_cleanup may call destructors
                                // via nested interpretation, which pushes/pops call_stack and
                                // can invalidate the pointer due to Vector reallocation)
                                frame = &vm->call_stack.back();

                                // Found a matching handler - jump to it
                                regs[handler.exception_reg] = reg_from_ptr(exception_ptr);
                                pc = func->code.data() + handler.handler_pc;
                                handler_found = true;
                                break;
                            }
                        }
                    }

                    if (handler_found) break;

                    // No handler in this frame - clean up ALL owned locals in this frame
                    execute_cleanup(vm, func, current_pc, UINT32_MAX, regs);

                    // Re-cache frame pointer (may have been invalidated by nested
                    // destructor calls during cleanup)
                    frame = &vm->call_stack.back();

                    // Pop frame and continue unwinding
                    u32 local_stack_base = frame->local_stack_base;
                    vm->call_stack.pop_back();
                    vm->register_top -= func->register_count;
                    vm->local_stack_top = local_stack_base;

                    if (vm->call_stack.empty()) {
                        // Unhandled exception - free exception object and set error
                        object_free(vm, exception_ptr);
                        vm->error = "Unhandled exception";
                        vm->running = false;
                        return false;
                    }

                    // Restore caller frame
                    frame = &vm->call_stack.back();
                    func = frame->func;
                    regs = frame->registers;
                    // frame->pc already contains the right PC from saved state
                }

                break;
            }

            case Opcode::CALL_EXC_MSG: {
                // For now, a simple stub - will be implemented when ExceptionRef.message() is needed
                void* exception_ptr = reg_as_ptr(regs[b]);
                if (exception_ptr) {
                    // Call the stored message function for this exception type
                    // For now, return a generic message
                    void* msg = string_alloc(vm, "exception", 9);
                    regs[a] = reg_from_ptr(msg);
                }
                break;
            }

            case Opcode::HALT:
                vm->running = false;
                return true;

            default:
                vm->error = "Unknown opcode";
                return false;
        }
    }

    return true;
}

}

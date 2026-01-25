#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/object.hpp"
#include "roxy/vm/array.hpp"
#include "roxy/vm/string.hpp"

#include <cmath>
#include <cassert>
#include <cstring>

namespace rx {

// Helper functions for type punning with untyped registers
inline i64 reg_as_i64(u64 r) { return static_cast<i64>(r); }
inline f64 reg_as_f64(u64 r) { f64 v; memcpy(&v, &r, sizeof(v)); return v; }
inline void* reg_as_ptr(u64 r) { return reinterpret_cast<void*>(r); }

inline u64 reg_from_i64(i64 v) { return static_cast<u64>(v); }
inline u64 reg_from_f64(f64 v) { u64 r; memcpy(&r, &v, sizeof(r)); return r; }
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

bool interpret(RoxyVM* vm) {
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

            // Float Arithmetic
            case Opcode::ADD_F:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) + reg_as_f64(regs[c]));
                break;

            case Opcode::SUB_F:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) - reg_as_f64(regs[c]));
                break;

            case Opcode::MUL_F:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) * reg_as_f64(regs[c]));
                break;

            case Opcode::DIV_F:
                regs[a] = reg_from_f64(reg_as_f64(regs[b]) / reg_as_f64(regs[c]));
                break;

            case Opcode::NEG_F:
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

            // Float Comparisons
            case Opcode::EQ_F:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) == reg_as_f64(regs[c]));
                break;

            case Opcode::NE_F:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) != reg_as_f64(regs[c]));
                break;

            case Opcode::LT_F:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) < reg_as_f64(regs[c]));
                break;

            case Opcode::LE_F:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) <= reg_as_f64(regs[c]));
                break;

            case Opcode::GT_F:
                regs[a] = reg_from_bool(reg_as_f64(regs[b]) > reg_as_f64(regs[c]));
                break;

            case Opcode::GE_F:
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
            case Opcode::I2F:
                regs[a] = reg_from_f64(static_cast<f64>(reg_as_i64(regs[b])));
                break;

            case Opcode::F2I:
                regs[a] = reg_from_i64(static_cast<i64>(reg_as_f64(regs[b])));
                break;

            case Opcode::I2B:
                regs[a] = reg_from_bool(reg_as_i64(regs[b]) != 0);
                break;

            case Opcode::B2I:
                regs[a] = reg_is_truthy(regs[b]) ? 1 : 0;
                break;

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
                    // Store result in R0 of register file
                    vm->register_file[0] = result;
                    vm->running = false;
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

                const BCFunction* callee = vm->module->functions[func_idx];

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

                // Copy arguments
                for (u8 i = 0; i < arg_count; i++) {
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

            // Stack Address
            case Opcode::STACK_ADDR: {
                // Format: STACK_ADDR dst, slot_offset
                // dst = pointer to local_stack[local_stack_base + slot_offset]
                u16 slot_offset = imm;
                u32* addr = vm->local_stack + frame->local_stack_base + slot_offset;
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
                } else {
                    // 64-bit field: read two consecutive slots (little-endian)
                    regs[a] = static_cast<u64>(field[0]) | (static_cast<u64>(field[1]) << 32);
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
                } else {
                    // 64-bit field: write two consecutive slots (little-endian)
                    field[0] = static_cast<u32>(val);
                    field[1] = static_cast<u32>(val >> 32);
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

            // Index Access
            case Opcode::GET_INDEX: {
                // Format: GET_INDEX dst, arr, idx
                // dst = arr[idx]
                void* arr = reg_as_ptr(regs[b]);
                i64 index = reg_as_i64(regs[c]);
                Value result;
                if (!array_get(arr, index, result, &vm->error)) {
                    return false;
                }
                regs[a] = result.as_u64();
                break;
            }

            case Opcode::SET_INDEX: {
                // Format: SET_INDEX arr, idx, val
                // arr[idx] = val
                void* arr = reg_as_ptr(regs[a]);
                i64 index = reg_as_i64(regs[b]);
                Value value = Value::from_u64(regs[c]);
                if (!array_set(arr, index, value, &vm->error)) {
                    return false;
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
                    ref_dec(vm, ptr);
                }
                break;
            }

            case Opcode::WEAK_CHECK: {
                // Format: WEAK_CHECK dst, ptr_reg, gen_reg
                // Check if weak reference is still valid using 64-bit generation
                void* ptr = reg_as_ptr(regs[b]);
                u64 gen = regs[c];

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

            // Debug
            case Opcode::NOP:
                break;

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

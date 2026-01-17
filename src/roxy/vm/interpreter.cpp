#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/object.hpp"

#include <cmath>
#include <cassert>

namespace rx {

// Helper to get constant from constant pool
static Value load_constant(const BCFunction* func, u16 index) {
    if (index >= func->constants.size()) {
        return Value::make_null();
    }

    const BCConstant& c = func->constants[index];
    switch (c.type) {
        case BCConstant::Null:
            return Value::make_null();
        case BCConstant::Bool:
            return Value::make_bool(c.as_bool);
        case BCConstant::Int:
            return Value::make_int(c.as_int);
        case BCConstant::Float:
            return Value::make_float(c.as_float);
        case BCConstant::String:
            // For now, strings are stored as pointers to constant data
            // TODO: Proper string object handling
            return Value::make_ptr(const_cast<char*>(c.as_string.data));
        default:
            return Value::make_null();
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
    Value* regs = frame->registers;

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
                regs[a] = Value::make_null();
                break;

            case Opcode::LOAD_TRUE:
                regs[a] = Value::make_bool(true);
                break;

            case Opcode::LOAD_FALSE:
                regs[a] = Value::make_bool(false);
                break;

            case Opcode::LOAD_INT:
                regs[a] = Value::make_int(static_cast<i16>(imm));
                break;

            case Opcode::LOAD_CONST:
                regs[a] = load_constant(func, imm);
                break;

            case Opcode::MOV:
                regs[a] = regs[b];
                break;

            // Integer Arithmetic
            case Opcode::ADD_I:
                regs[a] = Value::make_int(regs[b].as_int + regs[c].as_int);
                break;

            case Opcode::SUB_I:
                regs[a] = Value::make_int(regs[b].as_int - regs[c].as_int);
                break;

            case Opcode::MUL_I:
                regs[a] = Value::make_int(regs[b].as_int * regs[c].as_int);
                break;

            case Opcode::DIV_I:
                if (regs[c].as_int == 0) {
                    vm->error = "Division by zero";
                    return false;
                }
                regs[a] = Value::make_int(regs[b].as_int / regs[c].as_int);
                break;

            case Opcode::MOD_I:
                if (regs[c].as_int == 0) {
                    vm->error = "Division by zero";
                    return false;
                }
                regs[a] = Value::make_int(regs[b].as_int % regs[c].as_int);
                break;

            case Opcode::NEG_I:
                regs[a] = Value::make_int(-regs[b].as_int);
                break;

            // Float Arithmetic
            case Opcode::ADD_F:
                regs[a] = Value::make_float(regs[b].as_float + regs[c].as_float);
                break;

            case Opcode::SUB_F:
                regs[a] = Value::make_float(regs[b].as_float - regs[c].as_float);
                break;

            case Opcode::MUL_F:
                regs[a] = Value::make_float(regs[b].as_float * regs[c].as_float);
                break;

            case Opcode::DIV_F:
                regs[a] = Value::make_float(regs[b].as_float / regs[c].as_float);
                break;

            case Opcode::NEG_F:
                regs[a] = Value::make_float(-regs[b].as_float);
                break;

            // Bitwise Operations
            case Opcode::BIT_AND:
                regs[a] = Value::make_int(regs[b].as_int & regs[c].as_int);
                break;

            case Opcode::BIT_OR:
                regs[a] = Value::make_int(regs[b].as_int | regs[c].as_int);
                break;

            case Opcode::BIT_XOR:
                regs[a] = Value::make_int(regs[b].as_int ^ regs[c].as_int);
                break;

            case Opcode::BIT_NOT:
                regs[a] = Value::make_int(~regs[b].as_int);
                break;

            case Opcode::SHL:
                regs[a] = Value::make_int(regs[b].as_int << regs[c].as_int);
                break;

            case Opcode::SHR:
                regs[a] = Value::make_int(regs[b].as_int >> regs[c].as_int);
                break;

            case Opcode::USHR:
                regs[a] = Value::make_int(
                    static_cast<i64>(static_cast<u64>(regs[b].as_int) >> regs[c].as_int));
                break;

            // Integer Comparisons
            case Opcode::EQ_I:
                regs[a] = Value::make_bool(regs[b].as_int == regs[c].as_int);
                break;

            case Opcode::NE_I:
                regs[a] = Value::make_bool(regs[b].as_int != regs[c].as_int);
                break;

            case Opcode::LT_I:
                regs[a] = Value::make_bool(regs[b].as_int < regs[c].as_int);
                break;

            case Opcode::LE_I:
                regs[a] = Value::make_bool(regs[b].as_int <= regs[c].as_int);
                break;

            case Opcode::GT_I:
                regs[a] = Value::make_bool(regs[b].as_int > regs[c].as_int);
                break;

            case Opcode::GE_I:
                regs[a] = Value::make_bool(regs[b].as_int >= regs[c].as_int);
                break;

            case Opcode::LT_U:
                regs[a] = Value::make_bool(
                    static_cast<u64>(regs[b].as_int) < static_cast<u64>(regs[c].as_int));
                break;

            case Opcode::LE_U:
                regs[a] = Value::make_bool(
                    static_cast<u64>(regs[b].as_int) <= static_cast<u64>(regs[c].as_int));
                break;

            case Opcode::GT_U:
                regs[a] = Value::make_bool(
                    static_cast<u64>(regs[b].as_int) > static_cast<u64>(regs[c].as_int));
                break;

            case Opcode::GE_U:
                regs[a] = Value::make_bool(
                    static_cast<u64>(regs[b].as_int) >= static_cast<u64>(regs[c].as_int));
                break;

            // Float Comparisons
            case Opcode::EQ_F:
                regs[a] = Value::make_bool(regs[b].as_float == regs[c].as_float);
                break;

            case Opcode::NE_F:
                regs[a] = Value::make_bool(regs[b].as_float != regs[c].as_float);
                break;

            case Opcode::LT_F:
                regs[a] = Value::make_bool(regs[b].as_float < regs[c].as_float);
                break;

            case Opcode::LE_F:
                regs[a] = Value::make_bool(regs[b].as_float <= regs[c].as_float);
                break;

            case Opcode::GT_F:
                regs[a] = Value::make_bool(regs[b].as_float > regs[c].as_float);
                break;

            case Opcode::GE_F:
                regs[a] = Value::make_bool(regs[b].as_float >= regs[c].as_float);
                break;

            // Logical Operations
            case Opcode::NOT:
                regs[a] = Value::make_bool(!regs[b].is_truthy());
                break;

            case Opcode::AND:
                regs[a] = Value::make_bool(regs[b].is_truthy() && regs[c].is_truthy());
                break;

            case Opcode::OR:
                regs[a] = Value::make_bool(regs[b].is_truthy() || regs[c].is_truthy());
                break;

            // Type Conversions
            case Opcode::I2F:
                regs[a] = Value::make_float(static_cast<f64>(regs[b].as_int));
                break;

            case Opcode::F2I:
                regs[a] = Value::make_int(static_cast<i64>(regs[b].as_float));
                break;

            case Opcode::I2B:
                regs[a] = Value::make_bool(regs[b].as_int != 0);
                break;

            case Opcode::B2I:
                regs[a] = Value::make_int(regs[b].as_bool ? 1 : 0);
                break;

            // Control Flow
            case Opcode::JMP:
                pc += offset;
                break;

            case Opcode::JMP_IF:
                if (regs[a].is_truthy()) {
                    pc += offset;
                }
                break;

            case Opcode::JMP_IF_NOT:
                if (!regs[a].is_truthy()) {
                    pc += offset;
                }
                break;

            case Opcode::RET: {
                Value result = regs[a];

                // Save return register before popping frame
                u32 return_reg = frame->return_reg;

                // Pop current frame
                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;

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
                // Pop current frame
                vm->call_stack.pop_back();
                vm->register_top -= func->register_count;

                if (vm->call_stack.empty()) {
                    // Return from top-level function
                    vm->register_file[0] = Value::make_null();
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
                Value* callee_regs = &vm->register_file[vm->register_top];
                vm->register_top += callee->register_count;

                // Clear callee registers
                for (u32 i = 0; i < callee->register_count; i++) {
                    callee_regs[i] = Value::make_null();
                }

                // Copy arguments
                for (u8 i = 0; i < arg_count; i++) {
                    callee_regs[i] = regs[first_arg + i];
                }

                // Push new call frame
                CallFrame new_frame(callee, callee->code.data(), callee_regs, dst);
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

            // Field Access
            case Opcode::GET_FIELD: {
                // TODO: Implement struct field access
                vm->error = "GET_FIELD not implemented";
                return false;
            }

            case Opcode::SET_FIELD: {
                // TODO: Implement struct field access
                vm->error = "SET_FIELD not implemented";
                return false;
            }

            // Index Access
            case Opcode::GET_INDEX: {
                // TODO: Implement array index access
                vm->error = "GET_INDEX not implemented";
                return false;
            }

            case Opcode::SET_INDEX: {
                // TODO: Implement array index access
                vm->error = "SET_INDEX not implemented";
                return false;
            }

            // Object Lifecycle
            case Opcode::NEW_OBJ: {
                u32 type_id = imm;
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

                regs[a] = Value::make_ptr(data);
                break;
            }

            case Opcode::DEL_OBJ: {
                if (regs[a].is_ptr() && regs[a].as_ptr != nullptr) {
                    object_free(vm, regs[a].as_ptr);
                    regs[a] = Value::make_null();
                }
                break;
            }

            // Reference Counting
            case Opcode::REF_INC: {
                if (regs[a].is_ptr() && regs[a].as_ptr != nullptr) {
                    ref_inc(regs[a].as_ptr);
                }
                break;
            }

            case Opcode::REF_DEC: {
                if (regs[a].is_ptr() && regs[a].as_ptr != nullptr) {
                    ref_dec(vm, regs[a].as_ptr);
                }
                break;
            }

            case Opcode::WEAK_CHECK: {
                regs[a] = Value::make_bool(regs[b].is_weak_valid());
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

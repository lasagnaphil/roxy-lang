#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

#include <fmt/core.h>

namespace rx {

StringInterner VM::s_string_interner = {};

VM::VM() {
    m_stack_top = m_stack.data();
    init_uid_gen_state();
}

InterpretResult VM::run_module(Module& module) {
    m_frame_count = 1;

    // Allocate extra space for locals
    u32* stack = m_stack.data();
    m_stack_top = stack + module.chunk().get_locals_slot_size();

    m_frames[0] = {
        .chunk = &module.chunk(),
        .ip = module.chunk().m_bytecode.data(),
        .stack = stack,
    };

    m_cur_frame = &m_frames[0];
    return run();
}

InterpretResult VM::run() {
    s_string_interner.init();

    // Add frame
    m_cur_frame = &m_frames[m_frame_count - 1];

#ifdef DEBUG_TRACE_EXECUTION
    fmt::print("---- Debug Trace ----\n");
#endif
    for (;;) {
#ifdef DEBUG_TRACE_EXECUTION
        fmt::print("          ");
        u32 stack_size = m_stack_top - m_stack.data();
        u32 i = 0;
        while (i < stack_size) {
            switch (m_debug_stack_type[i]) {
                case PrimTypeKind::Bool:
                    if (m_stack[i++]) fmt::print("[ true ]");
                    else fmt::print("[ false ]");
                    break;
                case PrimTypeKind::U32:
                    fmt::print("[ {} ]", m_stack[i++]);
                    break;
                case PrimTypeKind::I32:
                    fmt::print("[ {} ]", (i32)m_stack[i++]);
                    break;
                case PrimTypeKind::U64: {
                    u64 value = (u64)m_stack[i++];
                    value |= (u64)m_stack[i++] << 32;
                    fmt::print("[ {} ]", value);
                    break;
                }
                case PrimTypeKind::I64: {
                    u64 value = (u64)m_stack[i++];
                    value |= (u64)m_stack[i++] << 32;
                    fmt::print("[ {} ]", value);
                    break;
                }
                case PrimTypeKind::F32: {
                    f32 value;
                    memcpy(&value, &m_stack[i++], sizeof(f32));
                    fmt::print("[ {} ]", value);
                    break;
                }
                case PrimTypeKind::F64: {
                    f64 value;
                    u64 value_u64 = (u64)m_stack[i++];
                    value_u64 |= (u64)m_stack[i++] << 32;
                    memcpy(&value, &value_u64, sizeof(f64));
                    fmt::print("[ {} ]", value);
                    break;
                }
                case PrimTypeKind::String: {
                    ObjString* value;
                    u64 value_u64 = (u64)m_stack[i++];
                    value_u64 |= (u64)m_stack[i++] << 32;
                    memcpy(&value, &value_u64, sizeof(f64));
                    fmt::print("[ {} ]", value->chars());
                    break;
                }
                default:
                    fmt::print("[ invalid ]");
                    i++;
                    break;
            }
            if (i >= stack_size) break;
            u32 size = PrimitiveType::get_kind_size(m_debug_stack_type[i]) >> 2;
            u32 alignment = PrimitiveType::get_kind_alignment(m_debug_stack_type[i]) >> 2;
            i = ((i + alignment - 1) & ~(alignment - 1)) + size;
        }
        fmt::print("\n");
        m_cur_frame->chunk->disassemble_instruction(
            (i32)(m_cur_frame->ip - m_cur_frame->chunk->m_bytecode.data()));
        fflush(stdout);
#endif

#define BINARY_OP(Type, Op) do { Type b = (Type)pop_##Type(); Type a = (Type)pop_##Type(); push_##Type(a Op b); } while(0);
#define BINARY_INTEGER_BR_S_OP(Op) do { u32 b = pop_u32(); u32 a = pop_u32(); u32 offset = (u32)read_u8(); if (a Op b) m_cur_frame->ip += offset; } while(0);
#define BINARY_INTEGER_BR_OP(Op) do { u32 b = pop_u32(); u32 a = pop_u32(); u32 offset = (u32)read_u32(); if (a Op b) m_cur_frame->ip += offset; } while(0);

        // TODO: check if these lambda functions can be inlined in Release mode
        auto iload = [this](u32 offset) {
#ifdef DEBUG_TRACE_EXECUTION
            u32 frame_start_idx = m_cur_frame->stack - m_stack.data();
            PrimTypeKind kind = m_debug_stack_type[frame_start_idx + offset];
            switch (kind) {
                case PrimTypeKind::U32: push_u32(get_local_u32(offset)); break;
                case PrimTypeKind::I32: push_i32((i32)get_local_u32(offset)); break;
                case PrimTypeKind::F32: push_f32(get_local_f32(offset)); break;
            }
#else
            push_u32(get_local_u32(offset));
#endif
        };

        auto lload = [this](u32 offset) {
            offset <<= 1;
#ifdef DEBUG_TRACE_EXECUTION
            u32 frame_start_idx = m_cur_frame->stack - m_stack.data();
            PrimTypeKind kind = m_debug_stack_type[frame_start_idx + offset];
            switch (kind) {
                case PrimTypeKind::U64: push_u64(get_local_u64(offset)); break;
                case PrimTypeKind::I64: push_i64((i32)get_local_u64(offset)); break;
                case PrimTypeKind::F64: push_f64(get_local_f64(offset)); break;
            }
#else
            push_u64(get_local_u64(offset));
#endif
        };

        OpCode inst = (OpCode)read_u8();
        switch (inst) {
        case OpCode::nop:
            break;
        case OpCode::iload_0: iload(0);
            break;
        case OpCode::iload_1: iload(1);
            break;
        case OpCode::iload_2: iload(2);
            break;
        case OpCode::iload_3: iload(3);
            break;
        case OpCode::iload: iload(read_u16());
            break;
        case OpCode::iload_s: iload(read_u8());
            break;
        case OpCode::istore_0: set_local_u32(0, pop_u32());
            break;
        case OpCode::istore_1: set_local_u32(1, pop_u32());
            break;
        case OpCode::istore_2: set_local_u32(2, pop_u32());
            break;
        case OpCode::istore_3: set_local_u32(3, pop_u32());
            break;
        case OpCode::istore: set_local_u32(read_u16(), pop_u32());
            break;
        case OpCode::istore_s: set_local_u32(read_u8(), pop_u32());
            break;
        case OpCode::lload_0: lload(0);
            break;
        case OpCode::lload_1: lload(1);
            break;
        case OpCode::lload_2: lload(2);
            break;
        case OpCode::lload_3: lload(3);
            break;
        case OpCode::lload: lload(read_u16());
            break;
        case OpCode::lload_s: lload(read_u8());
            break;
        case OpCode::lstore_0: set_local_u64(0, pop_u64());
            break;
        case OpCode::lstore_1: set_local_u64(2, pop_u64());
            break;
        case OpCode::lstore_2: set_local_u64(4, pop_u64());
            break;
        case OpCode::lstore_3: set_local_u64(6, pop_u64());
            break;
        case OpCode::lstore: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u16() << 1;
            set_local_u64(offset, value);
            break;
        }
        case OpCode::lstore_s: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u8() << 1;
            set_local_u64(offset, value);
            break;
        }
        case OpCode::rload_0: {
            Obj* value = get_local_ref(0);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rload_1: {
            Obj* value = get_local_ref(2);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rload_2: {
            Obj* value = get_local_ref(4);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rload_3: {
            Obj* value = get_local_ref(6);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rload: {
            u32 offset = (u32)read_u16() << 1;
            Obj* value = get_local_ref(offset);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rload_s: {
            u32 offset = (u32)read_u8() << 1;
            Obj* value = get_local_ref(offset);
            push_ref(value);
            value->incref();
            break;
        }
        case OpCode::rstore_0: {
            Obj* orig_value = get_local_ref(0);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(0, value);
            break;
        }
        case OpCode::rstore_1: {
            Obj* orig_value = get_local_ref(2);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(2, value);
            break;
        }
        case OpCode::rstore_2: {
            Obj* orig_value = get_local_ref(4);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(4, value);
            break;
        }
        case OpCode::rstore_3: {
            Obj* orig_value = get_local_ref(6);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(6, value);
            break;
        }
        case OpCode::rstore: {
            u32 offset = (u32)read_u16() << 1;
            Obj* orig_value = get_local_ref(offset);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(offset, value);
            break;
        }
        case OpCode::rstore_s: {
            u32 offset = (u32)read_u8() << 1;
            Obj* orig_value = get_local_ref(offset);
            if (orig_value) orig_value->decref();
            Obj* value = pop_ref();
            set_local_ref(offset, value);
            break;
        }
        case OpCode::iconst_m1: push_u32(-1);
            break;
        case OpCode::iconst_0: push_u32(0);
            break;
        case OpCode::iconst_1: push_u32(1);
            break;
        case OpCode::iconst_2: push_u32(2);
            break;
        case OpCode::iconst_3: push_u32(3);
            break;
        case OpCode::iconst_4: push_u32(4);
            break;
        case OpCode::iconst_5: push_u32(5);
            break;
        case OpCode::iconst_6: push_u32(6);
            break;
        case OpCode::iconst_7: push_u32(7);
            break;
        case OpCode::iconst_8: push_u32(8);
            break;
        case OpCode::iconst_s: push_u32(read_u8());
            break;
        case OpCode::iconst: push_u32(read_u32());
            break;
        case OpCode::lconst: push_u64(read_u64());
            break;
        case OpCode::fconst: push_f32(read_f32());
            break;
        case OpCode::dconst: push_f64(read_f64());
            break;
        case OpCode::idup: push_u32(top());
            break;
        case OpCode::ipop: pop_u32();
            break;
        case OpCode::ldup: push_u64(top_u64());
            break;
        case OpCode::lpop: pop_u64();
            break;
        case OpCode::call: {
            u16 offset = read_u16();
            auto fn_chunk = m_cur_frame->chunk->m_function_table[offset];
            u32 params_slot_size = fn_chunk->get_params_slot_size();

            m_cur_frame = &m_frames[m_frame_count++];
            m_cur_frame->chunk = fn_chunk;
            m_cur_frame->ip = fn_chunk->m_bytecode.data();
            m_cur_frame->stack = m_stack_top - params_slot_size;
            break;
        }
        case OpCode::callnative: {
            u16 offset = read_u16();
            auto fn_ptr = m_cur_frame->chunk->m_native_function_table[offset];
            auto arg_stack = ArgStack(m_stack_top);
            fn_ptr(&arg_stack);
            m_stack_top = arg_stack.top();
            break;
        }
        case OpCode::ret: {
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            break;
        }
        case OpCode::iret: {
            u32 ret_value_u32 = pop_u32();
#ifdef DEBUG_TRACE_EXECUTION
            u32 stack_top_idx = m_stack_top - m_stack.data();
            PrimTypeKind kind = m_debug_stack_type[stack_top_idx];
#endif
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
#ifdef DEBUG_TRACE_EXECUTION
            switch (kind) {
                case PrimTypeKind::U32: push_u32(ret_value_u32); break;
                case PrimTypeKind::I32: push_i32((i32)ret_value_u32); break;
                case PrimTypeKind::F32: {
                    f32 ret_value;
                    memcpy(&ret_value, &ret_value_u32, sizeof(f32));
                    push_f32(ret_value);
                    break;
                }
            }
#else
            push_u32(ret_value_u32);
#endif
            break;
        }
        case OpCode::lret: {
            u64 ret_value_u64 = pop_u64();
#ifdef DEBUG_TRACE_EXECUTION
            u32 stack_top_idx = m_stack_top - m_stack.data();
            PrimTypeKind kind = m_debug_stack_type[stack_top_idx];
#endif
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
#ifdef DEBUG_TRACE_EXECUTION
            switch (kind) {
                case PrimTypeKind::U32: push_u64(ret_value_u64); break;
                case PrimTypeKind::I32: push_i64((i64)ret_value_u64); break;
                case PrimTypeKind::F32: {
                    f64 ret_value;
                    memcpy(&ret_value, &ret_value_u64, sizeof(f64));
                    push_f64(ret_value);
                    break;
                }
            }
#else
            push_u64(ret_value_u64);
#endif
            break;
        }
        case OpCode::rret: {
            Obj* ret_value = pop_ref();
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                ret_value->decref();
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            push_ref(ret_value);
            break;
        }
        case OpCode::jmp_s: m_cur_frame->ip += read_u8();
            break;
        case OpCode::loop_s: m_cur_frame->ip -= read_u8();
            break;
        case OpCode::br_true_s: if (pop_u32()) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_false_s: if (!pop_u32()) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_icmpeq_s: BINARY_INTEGER_BR_S_OP(==);
            break;
        case OpCode::br_icmpne_s: BINARY_INTEGER_BR_S_OP(!=);
            break;
        case OpCode::br_icmpge_s: BINARY_INTEGER_BR_S_OP(>=);
            break;
        case OpCode::br_icmpgt_s: BINARY_INTEGER_BR_S_OP(>);
            break;
        case OpCode::br_icmple_s: BINARY_INTEGER_BR_S_OP(<=);
            break;
        case OpCode::br_icmplt_s: BINARY_INTEGER_BR_S_OP(<);
            break;
        case OpCode::br_eq_s: if (pop_i32() == 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_ne_s: if (pop_i32() != 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_ge_s: if (pop_i32() >= 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_gt_s: if (pop_i32() > 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_le_s: if (pop_i32() <= 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::br_lt_s: if (pop_i32() < 0) m_cur_frame->ip += read_u8();
            break;
        case OpCode::jmp: m_cur_frame->ip += read_u32();
            break;
        case OpCode::loop: m_cur_frame->ip -= read_u32();
            break;
        case OpCode::br_true: if (pop_u32()) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_false: if (!pop_u32()) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_icmpeq: BINARY_INTEGER_BR_OP(==);
            break;
        case OpCode::br_icmpne: BINARY_INTEGER_BR_OP(!=);
            break;
        case OpCode::br_icmpge: BINARY_INTEGER_BR_OP(>=);
            break;
        case OpCode::br_icmpgt: BINARY_INTEGER_BR_OP(>);
            break;
        case OpCode::br_icmple: BINARY_INTEGER_BR_OP(<=);
            break;
        case OpCode::br_icmplt: BINARY_INTEGER_BR_OP(<);
            break;
        case OpCode::br_eq: if (pop_i32() == 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_ne: if (pop_i32() != 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_ge: if (pop_i32() >= 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_gt: if (pop_i32() > 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_le: if (pop_i32() <= 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::br_lt: if (pop_i32() < 0) m_cur_frame->ip += read_u32();
            break;
        case OpCode::iadd: BINARY_OP(i32, +);
            break;
        case OpCode::isub: BINARY_OP(i32, -);
            break;
        case OpCode::imul: BINARY_OP(i32, *);
            break;
        case OpCode::uimul: BINARY_OP(u32, *);
            break;
        case OpCode::idiv: BINARY_OP(i32, /);
            break;
        case OpCode::uidiv: BINARY_OP(u32, /);
            break;
        case OpCode::irem: BINARY_OP(i32, %);
            break;
        case OpCode::uirem: BINARY_OP(u32, %);
            break;
        case OpCode::ladd: BINARY_OP(i64, +);
            break;
        case OpCode::lsub: BINARY_OP(i64, -);
            break;
        case OpCode::lmul: BINARY_OP(i64, *);
            break;
        case OpCode::ulmul: BINARY_OP(u64, *);
            break;
        case OpCode::ldiv: BINARY_OP(i64, /);
            break;
        case OpCode::uldiv: BINARY_OP(u64, /);
            break;
        case OpCode::lrem: BINARY_OP(i64, %);
            break;
        case OpCode::ulrem: BINARY_OP(u64, %);
            break;
        case OpCode::fadd: BINARY_OP(f32, +);
            break;
        case OpCode::fsub: BINARY_OP(f32, -);
            break;
        case OpCode::fmul: BINARY_OP(f32, *);
            break;
        case OpCode::fdiv: BINARY_OP(f32, /);
            break;
        case OpCode::dadd: BINARY_OP(f64, +);
            break;
        case OpCode::dsub: BINARY_OP(f64, -);
            break;
        case OpCode::dmul: BINARY_OP(f64, *);
            break;
        case OpCode::ddiv: BINARY_OP(f64, /);
            break;
        case OpCode::lcmp: {
            i64 b = pop_i64(); i64 a = pop_i64(); push_i32(a == b? 0 : (a < b? -1 : 1)); break;
        }
        case OpCode::fcmp: {
            f32 b = pop_f32(); f32 a = pop_f32(); push_i32(a == b? 0 : (a < b? -1 : 1)); break;
        }
        case OpCode::dcmp: {
            f64 b = pop_f64(); f64 a = pop_f64(); push_i32(a == b? 0 : (a < b? -1 : 1)); break;
        }
        case OpCode::ldstr: {
            u32 offset = read_u32();
            auto str = m_cur_frame->chunk->m_outer_module->string_table().get_string(offset);
            ObjString* obj_str = s_string_interner.create_string(str);
            Obj* obj = reinterpret_cast<Obj*>(obj_str);
            push_ref(obj);
            break;
        }
        default: return InterpretResult::RuntimeError;
        }
    }

interpreter_end:
    return InterpretResult::Ok;
}
}

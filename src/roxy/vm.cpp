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

// #define DEBUG_TRACE_EXECUTION

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
        for (u32* slot = m_stack.data(); slot < m_stack_top; slot++) {
            fmt::print("[ {} ]", *slot);
        }
        fmt::print("\n");
        m_cur_frame->chunk->disassemble_instruction(
            (i32)(m_cur_frame->ip - m_cur_frame->chunk->m_bytecode.data()));
        fflush(stdout);
#endif

#define BINARY_OP(Type, Op) do { Type b = (Type)pop_##Type(); Type a = (Type)pop_##Type(); push_##Type(a Op b); } while(0);
#define BINARY_INTEGER_BR_S_OP(Op) do { u32 b = pop_u32(); u32 a = pop_u32(); u32 offset = (u32)read_u8(); if (a Op b) m_cur_frame->ip += offset; } while(0);
#define BINARY_INTEGER_BR_OP(Op) do { u32 b = pop_u32(); u32 a = pop_u32(); u32 offset = (u32)read_u32(); if (a Op b) m_cur_frame->ip += offset; } while(0);

        OpCode inst = (OpCode)read_u8();
        switch (inst) {
        case OpCode::nop:
            break;
        case OpCode::iload_0: push_u32(get_local_u32(0));
            break;
        case OpCode::iload_1: push_u32(get_local_u32(1));
            break;
        case OpCode::iload_2: push_u32(get_local_u32(2));
            break;
        case OpCode::iload_3: push_u32(get_local_u32(3));
            break;
        case OpCode::iload: push_u32(get_local_u32(read_u16()));
            break;
        case OpCode::iload_s: push_u32(get_local_u32(read_u8()));
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
        case OpCode::lload_0: push_u64(get_local_u64(0));
            break;
        case OpCode::lload_1: push_u64(get_local_u64(2));
            break;
        case OpCode::lload_2: push_u64(get_local_u64(4));
            break;
        case OpCode::lload_3: push_u64(get_local_u64(6));
            break;
        case OpCode::lload: {
            u32 offset = (u32)read_u16() << 1;
            push_u64(get_local_u64(offset));
            break;
        }
        case OpCode::lload_s: {
            u32 offset = (u32)read_u8() << 1;
            push_u64(get_local_u64(offset));
            break;
        }
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
        case OpCode::dup: push_u32(top());
            break;
        case OpCode::pop: pop_u32();
            break;
        case OpCode::call: {
            u16 offset = read_u16();
            auto fn_chunk = m_cur_frame->chunk->m_function_table[offset];
            u32 locals_slot_size = fn_chunk->get_locals_slot_size();

            m_cur_frame = &m_frames[m_frame_count++];
            m_cur_frame->chunk = fn_chunk;
            m_cur_frame->ip = fn_chunk->m_bytecode.data();
            m_cur_frame->stack = m_stack_top - locals_slot_size;
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
            u32 ret_value = pop_u32();
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            push_u32(ret_value);
            break;
        }
        case OpCode::lret: {
            u64 ret_value = pop_u64();
            decref_locals();
            m_frame_count--;
            if (m_frame_count == 0) {
                goto interpreter_end;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            push_u64(ret_value);
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
        case OpCode::ldstr: {
            u32 offset = read_u32();
            auto str = m_cur_frame->chunk->m_outer_module->string_table().get_string(offset);
            ObjString* obj_str = s_string_interner.create_string(str);
            Obj* obj = reinterpret_cast<Obj*>(obj_str);
            push_ref(obj);
            // obj->incref();
            break;
        }
        default: return InterpretResult::RuntimeError;
        }
    }

interpreter_end:
    return InterpretResult::Ok;
}
}

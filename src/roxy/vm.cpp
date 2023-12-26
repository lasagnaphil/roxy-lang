#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

#include <fmt/core.h>

namespace rx {
VM::VM() {
    m_stack_top = m_stack.data();
}

InterpretResult VM::run_chunk(Chunk& chunk) {
    m_frame_count = 1;

    // Allocate extra space for locals
    m_stack_top += chunk.get_locals_slot_size();

    m_frames[0] = {
        .chunk = &chunk,
        .ip = chunk.m_bytecode.data(),
        .stack = m_stack_top,
    };

    m_cur_frame = &m_frames[0];
    return run();
}

#define DEBUG_TRACE_EXECUTION

InterpretResult VM::run() {
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
#define BINARY_INTEGER_BR_S_OP(Op) do { u32 a = pop_u32(); u32 b = pop_u32(); u32 offset = (u32)read_u8(); if (a Op b) m_cur_frame->ip += offset; } while(0);
#define BINARY_INTEGER_BR_OP(Op) do { u32 a = pop_u32(); u32 b = pop_u32(); u32 offset = (u32)read_u32(); if (a Op b) m_cur_frame->ip += offset; } while(0);

        OpCode inst = (OpCode)read_u8();
        switch (inst) {
        case OpCode::iload_0: push_u32(m_cur_frame->stack[0]);
            break;
        case OpCode::iload_1: push_u32(m_cur_frame->stack[1]);
            break;
        case OpCode::iload_2: push_u32(m_cur_frame->stack[2]);
            break;
        case OpCode::iload_3: push_u32(m_cur_frame->stack[3]);
            break;
        case OpCode::iload: push_u32(m_cur_frame->stack[read_u16()]);
            break;
        case OpCode::iload_s: push_u32(m_cur_frame->stack[read_u8()]);
            break;
        case OpCode::istore_0: m_cur_frame->stack[0] = pop_u32();
            break;
        case OpCode::istore_1: m_cur_frame->stack[1] = pop_u32();
            break;
        case OpCode::istore_2: m_cur_frame->stack[2] = pop_u32();
            break;
        case OpCode::istore_3: m_cur_frame->stack[3] = pop_u32();
            break;
        case OpCode::istore: m_cur_frame->stack[read_u16()] = pop_u32();
            break;
        case OpCode::istore_s: m_cur_frame->stack[read_u8()] = pop_u32();
            break;
        case OpCode::lload_0: {
            u64 value;
            memcpy(&value, m_cur_frame->stack, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_1: {
            u64 value;
            memcpy(&value, m_cur_frame->stack + 2, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_2: {
            u64 value;
            memcpy(&value, m_cur_frame->stack + 4, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_3: {
            u64 value;
            memcpy(&value, m_cur_frame->stack + 6, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload: {
            u32 offset = (u32)read_u16() << 1;
            u64 value;
            memcpy(&value, m_cur_frame->stack + offset, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_s: {
            u32 offset = (u32)read_u8() << 1;
            u64 value;
            memcpy(&value, m_cur_frame->stack + offset, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lstore_0: {
            u64 value = pop_u64();
            memcpy(m_cur_frame->stack, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_1: {
            u64 value = pop_u64();
            memcpy(m_cur_frame->stack + 2, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_2: {
            u64 value = pop_u64();
            memcpy(m_cur_frame->stack + 4, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_3: {
            u64 value = pop_u64();
            memcpy(m_cur_frame->stack + 6, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u16() << 1;
            memcpy(m_cur_frame->stack + offset, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_s: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u8() << 1;
            memcpy(m_cur_frame->stack + offset, &value, sizeof(u64));
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
            // TODO: Optimize this by creating a runtime-only table for fast lookup
            auto& fn_entry = m_cur_frame->chunk->m_function_table[read_u16()];
            auto fn_chunk = &fn_entry.chunk;
            u32 locals_slot_size = fn_chunk->get_locals_slot_size();
            u32 return_value_size = (fn_entry.type.ret->size + 3) / 4;

            m_cur_frame = &m_frames[m_frame_count++];
            m_cur_frame->chunk = fn_chunk;
            m_cur_frame->ip = fn_chunk->m_bytecode.data();
            m_cur_frame->stack = m_stack_top - locals_slot_size;
            break;
        }
        case OpCode::ret: {
            m_frame_count--;
            if (m_frame_count == 0) {
                return InterpretResult::Ok;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            break;
        }
        case OpCode::iret: {
            u32 ret_value = pop_u32();
            m_frame_count--;
            if (m_frame_count == 0) {
                return InterpretResult::Ok;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            push_u32(ret_value);
            break;
        }
        case OpCode::lret: {
            u64 ret_value = pop_u64();
            m_frame_count--;
            if (m_frame_count == 0) {
                return InterpretResult::Ok;
            }
            m_stack_top = m_cur_frame->stack;
            m_cur_frame = &m_frames[m_frame_count - 1];
            push_u64(ret_value);
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
        case OpCode::print: printf("%d\n", pop_u32()); // temp
            break;
        default: return InterpretResult::RuntimeError;
        }
    }

    return InterpretResult::Ok;
}
}

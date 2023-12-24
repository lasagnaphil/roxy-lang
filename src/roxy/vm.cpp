#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

#include <fmt/core.h>

namespace rx {
VM::VM() {
    m_stack_top = m_stack.data();
}

// #define ROXY_USE_SEPARATE_LOCALS

InterpretResult VM::run_chunk(Chunk& chunk) {
    m_frame_count = 1;

#ifdef ROXY_USE_SEPARATE_LOCALS
    // Allocate extra space for locals
    u32* locals = m_stack_top;
    m_stack_top += chunk.get_locals_slot_size();

    m_frames[0] = {
        .chunk = &chunk,
        .ip = chunk.m_bytecode.data(),
        .stack = m_stack_top,
        .locals = locals
    };
#else
    m_frames[0] = {
        .chunk = &chunk,
        .ip = chunk.m_bytecode.data(),
        .stack = m_stack_top,
        .locals = m_stack_top
    };
#endif

    m_cur_frame = &m_frames[0];
    return run();
}

#define DEBUG_TRACE_EXECUTION

InterpretResult VM::run() {
    // Add frame
    CallFrame& frame = m_frames[m_frame_count - 1];

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
        frame.chunk->disassemble_instruction(
            (i32)(frame.ip - frame.chunk->m_bytecode.data()));
        fflush(stdout);
#endif

#define BINARY_OP(Type, Op) do { Type b = (Type)pop_##Type(); Type a = (Type)pop_##Type(); push_##Type(a Op b); } while(0);
#define BINARY_INTEGER_BR_S_OP(Op) do { u32 a = pop_u32(); u32 b = pop_u32(); u32 offset = (u32)read_u8(); if (a Op b) frame.ip += offset; } while(0);
#define BINARY_INTEGER_BR_OP(Op) do { u32 a = pop_u32(); u32 b = pop_u32(); u32 offset = (u32)read_u32(); if (a Op b) frame.ip += offset; } while(0);

        OpCode inst = (OpCode)read_u8();
        switch (inst) {
        case OpCode::iload_0: push_u32(frame.locals[0]);
            break;
        case OpCode::iload_1: push_u32(frame.locals[1]);
            break;
        case OpCode::iload_2: push_u32(frame.locals[2]);
            break;
        case OpCode::iload_3: push_u32(frame.locals[3]);
            break;
        case OpCode::iload: push_u32(frame.locals[read_u16()]);
            break;
        case OpCode::iload_s: push_u32(frame.locals[read_u8()]);
            break;
#ifdef ROXY_USE_SEPARATE_LOCALS
        case OpCode::istore_0: frame.locals[0] = pop_u32();
            break;
        case OpCode::istore_1: frame.locals[1] = pop_u32();
            break;
        case OpCode::istore_2: frame.locals[2] = pop_u32();
            break;
        case OpCode::istore_3: frame.locals[3] = pop_u32();
            break;
        case OpCode::istore: frame.locals[read_u16()] = pop_u32();
            break;
        case OpCode::istore_s: frame.locals[read_u8()] = pop_u32();
            break;
#else
        case OpCode::istore_0:
        case OpCode::istore_1:
        case OpCode::istore_2:
        case OpCode::istore_3:
        case OpCode::istore:
        case OpCode::istore_s:
            // do nothing
            break;
#endif
        case OpCode::lload_0: {
            u64 value;
            memcpy(&value, frame.locals, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_1: {
            u64 value;
            memcpy(&value, frame.locals + 2, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_2: {
            u64 value;
            memcpy(&value, frame.locals + 4, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_3: {
            u64 value;
            memcpy(&value, frame.locals + 6, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload: {
            u32 offset = (u32)read_u16() << 1;
            u64 value;
            memcpy(&value, frame.locals + offset, sizeof(u64));
            push_u64(value);
            break;
        }
        case OpCode::lload_s: {
            u32 offset = (u32)read_u8() << 1;
            u64 value;
            memcpy(&value, frame.locals + offset, sizeof(u64));
            push_u64(value);
            break;
        }
#ifdef ROXY_USE_SEPARATE_LOCALS
        case OpCode::lstore_0: {
            u64 value = pop_u64();
            memcpy(frame.locals, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_1: {
            u64 value = pop_u64();
            memcpy(frame.locals + 2, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_2: {
            u64 value = pop_u64();
            memcpy(frame.locals + 4, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_3: {
            u64 value = pop_u64();
            memcpy(frame.locals + 6, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u16() << 1;
            memcpy(frame.locals + offset, &value, sizeof(u64));
            break;
        }
        case OpCode::lstore_s: {
            u64 value = pop_u64();
            u32 offset = (u32)read_u8() << 1;
            memcpy(frame.locals + offset, &value, sizeof(u64));
            break;
        }
#else
        case OpCode::lstore_0:
        case OpCode::lstore_1:
        case OpCode::lstore_2:
        case OpCode::lstore_3:
        case OpCode::lstore:
        case OpCode::lstore_s:
            // do nothing
            break;
#endif
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
        case OpCode::jmp_s: frame.ip += read_u8();
            break;
        case OpCode::br_true_s: if (pop_u32()) frame.ip += read_u8();
            break;
        case OpCode::br_false_s: if (!pop_u32()) frame.ip += read_u8();
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
        case OpCode::jmp: frame.ip += read_u32();
            break;
        case OpCode::br_true: if (pop_u32()) frame.ip += read_u32();
            break;
        case OpCode::br_false: if (!pop_u32()) frame.ip += read_u32();
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
        case OpCode::ret: return InterpretResult::Ok;
        default: return InterpretResult::RuntimeError;
        }
    }

    return InterpretResult::Ok;
}
}

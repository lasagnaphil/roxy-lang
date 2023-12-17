#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

#include <fmt/core.h>

namespace rx {

VM::VM() {
    m_stack_top = m_stack.data();
}

InterpretResult VM::run_chunk(Chunk& chunk) {
    m_frame_count = 1;
    m_frames[0] = {
            .ip = chunk.m_bytecode.data(),
            .slots = m_stack_top
    };
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
        /*
        frame->closure->function->chunk.disassemble_instruction(
            (int32_t)(frame->ip - frame->closure->function->chunk.m_code.data()));
        fflush(stdout);
        */
#endif

#define BINARY_OP(Type, Op) do { Type b = (Type)pop_##Type(); Type a = (Type)pop_##Type(); push_##Type(a Op b); } while(0);

        OpCode inst = (OpCode) read_u8();
        switch (inst) {
            case OpCode::iload_0: push_u32(frame.slots[0]); break;
            case OpCode::iload_1: push_u32(frame.slots[1]); break;
            case OpCode::iload_2: push_u32(frame.slots[2]); break;
            case OpCode::iload_3: push_u32(frame.slots[3]); break;
            case OpCode::istore_0: frame.slots[0] = pop_u32(); break;
            case OpCode::istore_1: frame.slots[1] = pop_u32(); break;
            case OpCode::istore_2: frame.slots[2] = pop_u32(); break;
            case OpCode::istore_3: frame.slots[3] = pop_u32(); break;
            case OpCode::iload: push_u32(frame.slots[read_u16()]); break;
            case OpCode::iload_s: push_u32(frame.slots[read_u8()]); break;
            case OpCode::istore: frame.slots[read_u16()] = pop_u32(); break;
            case OpCode::istore_s: frame.slots[read_u8()] = pop_u32(); break;
            case OpCode::iconst_m1: push_u32(-1); break;
            case OpCode::iconst_0: push_u32(0); break;
            case OpCode::iconst_1: push_u32(1); break;
            case OpCode::iconst_2: push_u32(2); break;
            case OpCode::iconst_3: push_u32(3); break;
            case OpCode::iconst_4: push_u32(4); break;
            case OpCode::iconst_5: push_u32(5); break;
            case OpCode::iconst_6: push_u32(6); break;
            case OpCode::iconst_7: push_u32(7); break;
            case OpCode::iconst_8: push_u32(8); break;
            case OpCode::iconst_S: push_u32(read_u8()); break;
            case OpCode::iconst: push_u32(read_u32()); break;
            case OpCode::lconst: push_u64(read_u64()); break;
            case OpCode::fconst: push_f32(read_f32()); break;
            case OpCode::dconst: push_f64(read_f64()); break;
            case OpCode::dup: push_u32(top()); break;
            case OpCode::pop: pop_u32(); break;
            case OpCode::iadd: BINARY_OP(i32, +); break;
            case OpCode::isub: BINARY_OP(i32, -); break;
            case OpCode::imul: BINARY_OP(i32, *); break;
            case OpCode::uimul: BINARY_OP(u32, *); break;
            case OpCode::idiv: BINARY_OP(i32, /); break;
            case OpCode::uidiv: BINARY_OP(u32, /); break;
            case OpCode::irem: BINARY_OP(i32, %); break;
            case OpCode::uirem: BINARY_OP(u32, %); break;
            case OpCode::ladd: BINARY_OP(i64, +); break;
            case OpCode::lsub: BINARY_OP(i64, -); break;
            case OpCode::lmul: BINARY_OP(i64, *); break;
            case OpCode::ulmul: BINARY_OP(u64, *); break;
            case OpCode::ldiv: BINARY_OP(i64, /); break;
            case OpCode::uldiv: BINARY_OP(u64, /); break;
            case OpCode::lrem: BINARY_OP(i64, %); break;
            case OpCode::ulrem: BINARY_OP(u64, %); break;
            case OpCode::fadd: BINARY_OP(f32, +); break;
            case OpCode::fsub: BINARY_OP(f32, -); break;
            case OpCode::fmul: BINARY_OP(f32, *); break;
            case OpCode::fdiv: BINARY_OP(f32, /); break;
            case OpCode::dadd: BINARY_OP(f64, +); break;
            case OpCode::dsub: BINARY_OP(f64, -); break;
            case OpCode::dmul: BINARY_OP(f64, *); break;
            case OpCode::ddiv: BINARY_OP(f64, /); break;
            case OpCode::print: printf("%d\n", pop_u32()); break; // temp
            case OpCode::ret: return InterpretResult::Ok;
        }
    }

    return InterpretResult::Ok;
}

}
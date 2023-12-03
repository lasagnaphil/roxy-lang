#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

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
    return run();
}

InterpretResult VM::run() {
    // Add frame
    CallFrame& frame = m_frames[m_frame_count - 1];

    for (;;) {
        OpCode inst = (OpCode) read_u8();
        switch (inst) {
            case OpCode::ildc_m1: push(-1); break;
            case OpCode::ildc_0: push(0); break;
            case OpCode::ildc_1: push(1); break;
            case OpCode::ildc_2: push(2); break;
            case OpCode::ildc_3: push(3); break;
            case OpCode::ildc_4: push(4); break;
            case OpCode::ildc_5: push(5); break;
            case OpCode::ildc_6: push(6); break;
            case OpCode::ildc_7: push(7); break;
            case OpCode::ildc_8: push(8); break;
            case OpCode::ildc_S: push(read_u8()); break;
            case OpCode::ildc: push(read_u32()); break;
            case OpCode::lldc: push_u64(read_u64()); break;
            case OpCode::fldc: push(read_u32()); break;
            case OpCode::dldc: push_u64(read_u64()); break;
            case OpCode::dup: push(top()); break;
            case OpCode::pop: pop(); break;
            case OpCode::iadd: push(pop() + pop()); break;
            case OpCode::isub: push(pop() - pop()); break;
            case OpCode::imul: push((i32)pop() * (i32)pop()); break;
            case OpCode::uimul: push(pop() * pop()); break;
            case OpCode::idiv: push((i32)pop() / (i32)pop()); break;
            case OpCode::uidiv: push(pop() / pop()); break;
            case OpCode::irem: push((i32)pop() % (i32)pop()); break;
            case OpCode::uirem: push(pop() % pop()); break;
            case OpCode::ladd: push_u64(pop_u64() + pop_u64()); break;
            case OpCode::lsub: push_u64(pop_u64() - pop_u64()); break;
            case OpCode::lmul: push_u64((i64)pop_u64() * (i64)pop_u64()); break;
            case OpCode::ulmul: push_u64(pop_u64() * pop_u64()); break;
            case OpCode::ldiv: push_u64((i64)pop_u64() / (i64)pop_u64()); break;
            case OpCode::uldiv: push_u64(pop_u64() / pop_u64()); break;
            case OpCode::lrem: push_u64((i64)pop_u64() % (i64)pop_u64()); break;
            case OpCode::ulrem: push_u64(pop_u64() % pop_u64()); break;
            case OpCode::fadd: push_f32(pop_f32() + pop_f32()); break;
            case OpCode::fsub: push_f32(pop_f32() - pop_f32()); break;
            case OpCode::fmul: push_f32(pop_f32() * pop_f32()); break;
            case OpCode::fdiv: push_f32(pop_f32() / pop_f32()); break;
            case OpCode::dadd: push_f64(pop_f64() * pop_f64()); break;
            case OpCode::dsub: push_f64(pop_f64() / pop_f64()); break;
            case OpCode::dmul: push_f64(pop_f64() * pop_f64()); break;
            case OpCode::ddiv: push_f64(pop_f64() / pop_f64()); break;

            case OpCode::ret: return InterpretResult::Ok;
        }
    }

    return InterpretResult::Ok;
}

}
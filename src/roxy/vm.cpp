#include "roxy/vm.hpp"
#include "roxy/opcode.hpp"

namespace rx {

VM::VM() {
    m_stack_top = m_stack.data();
}

InterpretResult VM::run_bytecode(Span<u8> bytecode) {
    m_frame_count = 1;
    m_frames[0] = {
            .ip = bytecode.data(),
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
            case OpCode::LdC_i4_M1: push(-1); break;
            case OpCode::LdC_i4_0: push(0); break;
            case OpCode::LdC_i4_1: push(1); break;
            case OpCode::LdC_i4_2: push(2); break;
            case OpCode::LdC_i4_3: push(3); break;
            case OpCode::LdC_i4_4: push(4); break;
            case OpCode::LdC_i4_5: push(5); break;
            case OpCode::LdC_i4_6: push(6); break;
            case OpCode::LdC_i4_7: push(7); break;
            case OpCode::LdC_i4_8: push(8); break;
            case OpCode::LdC_i4_S: push(read_u8()); break;
            case OpCode::LdC_i4: push(read_u32()); break;
            case OpCode::LdC_i8: push_u64(read_u64()); break;
            case OpCode::LdC_r4: push(read_u32()); break;
            case OpCode::LdC_r8: push_u64(read_u64()); break;
            case OpCode::Dup: push(top()); break;
            case OpCode::Pop: pop(); break;
            case OpCode::Add_i4: push(pop() + pop()); break;
            case OpCode::Sub_i4: push(pop() - pop()); break;
            case OpCode::Mul_i4: push((i32)pop() * (i32)pop()); break;
            case OpCode::Mul_u4: push(pop() * pop()); break;
            case OpCode::Div_i4: push((i32)pop() / (i32)pop()); break;
            case OpCode::Div_u4: push(pop() / pop()); break;
            case OpCode::Rem_i4: push((i32)pop() % (i32)pop()); break;
            case OpCode::Rem_u4: push(pop() % pop()); break;
            case OpCode::Add_i8: push_u64(pop_u64() + pop_u64()); break;
            case OpCode::Sub_i8: push_u64(pop_u64() - pop_u64()); break;
            case OpCode::Mul_i8: push_u64((i64)pop_u64() * (i64)pop_u64()); break;
            case OpCode::Mul_u8: push_u64(pop_u64() * pop_u64()); break;
            case OpCode::Div_i8: push_u64((i64)pop_u64() / (i64)pop_u64()); break;
            case OpCode::Div_u8: push_u64(pop_u64() / pop_u64()); break;
            case OpCode::Rem_i8: push_u64((i64)pop_u64() % (i64)pop_u64()); break;
            case OpCode::Rem_u8: push_u64(pop_u64() % pop_u64()); break;
            case OpCode::Add_r4: push_f32(pop_f32() + pop_f32()); break;
            case OpCode::Sub_r4: push_f32(pop_f32() - pop_f32()); break;
            case OpCode::Mul_r4: push_f32(pop_f32() * pop_f32()); break;
            case OpCode::Div_r4: push_f32(pop_f32() / pop_f32()); break;
            case OpCode::Add_r8: push_f64(pop_f64() * pop_f64()); break;
            case OpCode::Sub_r8: push_f64(pop_f64() / pop_f64()); break;
            case OpCode::Mul_r8: push_f64(pop_f64() * pop_f64()); break;
            case OpCode::Div_r8: push_f64(pop_f64() / pop_f64()); break;
        }
    }

    return InterpretResult::Ok;
}

}
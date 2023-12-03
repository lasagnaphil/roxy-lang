#pragma once

#include "roxy/core/array.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler.hpp"

#include <string_view>

namespace rx {

struct Obj;

enum class InterpretResult {
    Ok,
    CompileError,
    RuntimeError
};

struct CallFrame {
    u8* ip;
    u32* slots;
};

class VM {
public:
    static constexpr u32 MaxFrameSize = 64;
    static constexpr u32 MaxStackSize = MaxFrameSize * (UINT8_MAX + 1);

    VM();

    InterpretResult run_chunk(Chunk& chunk);

private:

    InterpretResult run();

    inline void push(u32 value) {
        *m_stack_top = value;
        m_stack_top++;
    }

    inline void push_u64(u64 value) {
        *m_stack_top++ = value;
        *m_stack_top++ = value >> 32;
    }

    inline void push_f32(f32 value) {
        u32 value_u32;
        memcpy(&value_u32, &value, sizeof(u32));
        return push(value_u32);
    }

    inline void push_f64(f64 value) {
        u64 value_u64;
        memcpy(&value_u64, &value, sizeof(u64));
        return push_u64(value_u64);
    }

    inline u32 top() {
        return *m_stack_top;
    }

    inline u32 pop() {
        return *m_stack_top--;
    }

    inline u64 pop_u64() {
        u64 value = *m_stack_top--;
        value |= (u64)*m_stack_top-- << 32;
        return value;
    }

    inline f32 pop_f32() {
        u32 value = pop();
        f32 value_f32;
        memcpy(&value_f32, &value, sizeof(f32));
        return value_f32;
    }

    inline f64 pop_f64() {
        u64 value = pop_u64();
        f64 value_f64;
        memcpy(&value_f64, &value, sizeof(f64));
        return value_f64;
    }

    inline u8 read_u8() { return *m_cur_frame->ip++; }
    inline u16 read_u16() {
        u16 value = (m_cur_frame->ip[0] << 8) | m_cur_frame->ip[1];
        *m_cur_frame->ip += 2;
        return value;
    }
    inline u32 read_u32() {
        u32 value;
        memcpy(&value, m_cur_frame->ip, sizeof(u32));
        *m_cur_frame->ip += 4;
        return value;
    }
    inline u64 read_u64() {
        u64 value;
        memcpy(&value, m_cur_frame->ip, sizeof(u64));
        *m_cur_frame->ip += 8;
        return value;
    }
    inline f32 read_f32() {
        f32 value;
        memcpy(&value, m_cur_frame->ip, sizeof(f32));
        *m_cur_frame->ip += 4;
        return value;
    }
    inline f64 read_f64() {
        f64 value;
        memcpy(&value, m_cur_frame->ip, sizeof(f64));
        *m_cur_frame->ip += 8;
        return value;
    }

    Array<CallFrame, MaxFrameSize> m_frames;
    u32 m_frame_count = 0;

    Array<u32, MaxStackSize> m_stack;
    u32* m_stack_top = nullptr;

    CallFrame* m_cur_frame;

    ConstantTable m_constant_table;
};

}
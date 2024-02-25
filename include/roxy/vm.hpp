#pragma once

#include "roxy/core/array.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler.hpp"

#include <string_view>

#define DEBUG_TRACE_EXECUTION

namespace rx {

struct Obj;

enum class InterpretResult {
    Ok,
    CompileError,
    RuntimeError
};

struct CallFrame {
    Chunk* chunk;
    u8* ip;
    u32* stack;
};

class VM {
public:
    static constexpr u32 MaxFrameSize = 64;
    static constexpr u32 MaxStackSize = MaxFrameSize * (UINT8_MAX + 1);

    VM();

    InterpretResult run_module(Module& module);

    static StringInterner& get_global_string_interner() {
        return s_string_interner;
    }

private:

    InterpretResult run();

    inline void push_i32(u32 value) { push_u32(value); }
    inline void push_i64(u64 value) { push_u64(value); }

    inline void push_u32(u32 value) {
#ifdef DEBUG_TRACE_EXECUTION
        u32 stack_idx = m_stack_top - m_stack.data();
        m_debug_stack_type[stack_idx] = PrimTypeKind::U32;
#endif
        *m_stack_top = value;
        m_stack_top++;
    }

    inline void push_u64(u64 value) {
#ifdef DEBUG_TRACE_EXECUTION
        u32 stack_idx = m_stack_top - m_stack.data();
        m_debug_stack_type[stack_idx] = PrimTypeKind::U64;
        m_debug_stack_type[stack_idx + 1] = PrimTypeKind::Void;
#endif
        m_stack_top[0] = value;
        m_stack_top[1] = value >> 32;
        m_stack_top += 2;
    }

    inline void push_f32(f32 value) {
#ifdef DEBUG_TRACE_EXECUTION
        u32 stack_idx = m_stack_top - m_stack.data();
        m_debug_stack_type[stack_idx] = PrimTypeKind::F32;
#endif
        u32 value_u32;
        memcpy(&value_u32, &value, sizeof(u32));
        *m_stack_top = value_u32;
        m_stack_top++;
    }

    inline void push_f64(f64 value) {
#ifdef DEBUG_TRACE_EXECUTION
        u32 stack_idx = m_stack_top - m_stack.data();
        m_debug_stack_type[stack_idx] = PrimTypeKind::F64;
        m_debug_stack_type[stack_idx + 1] = PrimTypeKind::Void;
#endif
        u64 value_u64;
        memcpy(&value_u64, &value, sizeof(u64));
        m_stack_top[0] = value_u64;
        m_stack_top[1] = value_u64 >> 32;
        m_stack_top += 2;
    }

    inline void push_ref(Obj* ref) {
#ifdef DEBUG_TRACE_EXECUTION
        u32 stack_idx = m_stack_top - m_stack.data();
        m_debug_stack_type[stack_idx] = PrimTypeKind::F64;
        m_debug_stack_type[stack_idx + 1] = PrimTypeKind::Void;
#endif
        u64 value_u64;
        memcpy(&value_u64, &ref, sizeof(u64));
        m_stack_top[0] = value_u64;
        m_stack_top[1] = value_u64 >> 32;
        m_stack_top += 2;
    }

    inline u32 top() {
        return *m_stack_top;
    }

    inline i32 pop_i32() { return (i32)pop_u32(); }
    inline i64 pop_i64() { return (i64)pop_u64(); }

    inline u32 pop_u32() {
        m_stack_top--;
        return *m_stack_top;
    }

    inline u64 pop_u64() {
        m_stack_top -= 2;
        u64 value = (u64)m_stack_top[0];
        value |= (u64)m_stack_top[1] << 32;
        return value;
    }

    inline f32 pop_f32() {
        u32 value = pop_u32();
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

    inline Obj* pop_ref() {
        u64 value = pop_u64();
        return reinterpret_cast<Obj*>(static_cast<intptr_t>(value));
    }

    inline u8 read_u8() { return *m_cur_frame->ip++; }
    inline u16 read_u16() {
        u16 value = m_cur_frame->ip[0] | (m_cur_frame->ip[1] << 8);
        m_cur_frame->ip += 2;
        return value;
    }
    inline u32 read_u32() {
        u32 value;
        memcpy(&value, m_cur_frame->ip, sizeof(u32));
        m_cur_frame->ip += 4;
        return value;
    }
    inline u64 read_u64() {
        u64 value;
        memcpy(&value, m_cur_frame->ip, sizeof(u64));
        m_cur_frame->ip += 8;
        return value;
    }
    inline f32 read_f32() {
        f32 value;
        memcpy(&value, m_cur_frame->ip, sizeof(f32));
        m_cur_frame->ip += 4;
        return value;
    }
    inline f64 read_f64() {
        f64 value;
        memcpy(&value, m_cur_frame->ip, sizeof(f64));
        m_cur_frame->ip += 8;
        return value;
    }

    inline u32 get_local_u32(u32 offset) {
        return m_cur_frame->stack[offset];
    }

    inline u64 get_local_u64(u32 offset) {
        u64 value;
        memcpy(&value, m_cur_frame->stack + offset, sizeof(u64));
        return value;
    }

    inline f32 get_local_f32(u32 offset) {
        u32 value_u32 = get_local_u32(offset);
        f32 value;
        memcpy(&value, &value_u32, sizeof(f32));
        return value;
    }

    inline f64 get_local_f64(u32 offset) {
        u64 value_u64 = get_local_u64(offset);
        f64 value;
        memcpy(&value, &value_u64, sizeof(f64));
        return value;
    }

    inline Obj* get_local_ref(u32 offset) {
        Obj* obj;
        memcpy(&obj, m_cur_frame->stack + offset, sizeof(Obj*));
        return obj;
    }

    inline void set_local_u32(u32 offset, u32 value) {
        m_cur_frame->stack[offset] = value;
    }

    inline void set_local_u64(u32 offset, u64 value) {
        memcpy(m_cur_frame->stack + offset, &value, sizeof(u64));
    }

    inline void set_local_ref(u32 offset, Obj* obj) {
        memcpy(m_cur_frame->stack + offset, &obj, sizeof(Obj*));
    }

    inline void decref_locals() {
        for (u16 offset : m_cur_frame->chunk->m_ref_local_offsets) {
            Obj* value = get_local_ref(offset);
            if (value) {
                value->decref();
            }
        }
    }

    Array<CallFrame, MaxFrameSize> m_frames;
    u32 m_frame_count = 0;

    Array<u32, MaxStackSize> m_stack;
    u32* m_stack_top = nullptr;

    Array<PrimTypeKind, MaxStackSize> m_debug_stack_type;

    CallFrame* m_cur_frame;

    static StringInterner s_string_interner;
};

}
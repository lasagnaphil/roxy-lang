#pragma once

#include "roxy/core/types.hpp"

#include <cassert>
#include <utility>

namespace rx {

class BumpAllocator {
public:
    BumpAllocator(u64 capacity) : m_start(nullptr), m_current(nullptr), m_capacity(0) {
        assert(capacity >= 64);
        reallocate(capacity);
    }

    u8* alloc_bytes(u64 size, u64 align) {
        assert(align > 0);
        assert((align & (align - 1)) == 0); // power of two

        u8* aligned = (u8*)((uintptr_t)(m_current + align - 1) & ~(align - 1));

        // TODO: There is a potential UB when size is really large and the addition overflows.
        //  Probably use __builtin_add_overflow intrinsic (or MSVC equivalent) to optimize bounds checking.
        u8* new_ptr = aligned + size;

        if (new_ptr - m_current > m_capacity) {
            reallocate(m_capacity << 1);
        }
        u8* ptr = aligned;
        m_current = aligned + size;
        return ptr;
    }

    template <typename T, typename ... Args>
    T* emplace(Args&&... args) {
        u8* ptr = alloc_bytes(sizeof(T), alignof(T));
        new (ptr) T(std::forward<Args>(args)...);
        return reinterpret_cast<T*>(ptr);
    }

private:
    void reallocate(u64 new_capacity) {
        u8* new_ptr = reinterpret_cast<u8*>(malloc(new_capacity));
        u8* new_addr = new_ptr + (m_current - m_start);
        if (m_start) {
            memcpy(new_ptr, m_start, m_capacity);
            free(m_start);
        }
        m_start = new_ptr;
        m_current = new_addr;
        m_capacity = new_capacity;
    }
private:
    u8* m_start;
    u8* m_current;
    u64 m_capacity;
};

}

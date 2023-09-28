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

    u8* alloc_bytes(u64 num_bytes) {
        if (m_current - m_start + num_bytes > m_capacity) {
            reallocate(2 * m_capacity);
        }
        u8* ptr = m_current;
        m_current += num_bytes;
        return ptr;
    }

    template <typename T, typename ... Args>
    T* emplace(Args&&... args) {
        u8* ptr = alloc_bytes(sizeof(T));
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

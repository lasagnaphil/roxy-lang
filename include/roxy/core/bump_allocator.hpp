#pragma once

#include "roxy/core/types.hpp"

#include <cassert>
#include <cstring>
#include <utility>

namespace rx {

// Chunk-based bump allocator that never frees memory until destruction.
// This ensures pointers remain valid even when new chunks are allocated.
class BumpAllocator {
    struct Chunk {
        Chunk* next;
        u64 capacity;
        u64 used;
        // Data follows immediately after this header
        u8* data() { return reinterpret_cast<u8*>(this + 1); }
    };

public:
    BumpAllocator(u64 initial_capacity) : m_head(nullptr), m_current(nullptr) {
        assert(initial_capacity >= 64);
        m_current = m_head = allocate_chunk(initial_capacity);
    }

    ~BumpAllocator() {
        Chunk* chunk = m_head;
        while (chunk) {
            Chunk* next = chunk->next;
            free(chunk);
            chunk = next;
        }
    }

    u8* alloc_bytes(u64 size, u64 align) {
        assert(align > 0);
        assert((align & (align - 1)) == 0); // power of two

        u8* base = m_current->data() + m_current->used;
        u8* aligned = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(base) + align - 1) & ~(align - 1));
        u64 padding = static_cast<u64>(aligned - base);
        u64 total_size = padding + size;

        // Check if current chunk has enough space
        if (m_current->used + total_size > m_current->capacity) {
            // Allocate a new chunk (at least double the size, or enough for this allocation)
            u64 new_capacity = m_current->capacity * 2;
            if (new_capacity < size + align) {
                new_capacity = size + align;
            }
            Chunk* new_chunk = allocate_chunk(new_capacity);
            new_chunk->next = nullptr;
            m_current->next = new_chunk;
            m_current = new_chunk;

            // Recalculate alignment in new chunk
            base = m_current->data();
            aligned = reinterpret_cast<u8*>((reinterpret_cast<uintptr_t>(base) + align - 1) & ~(align - 1));
            padding = static_cast<u64>(aligned - base);
            total_size = padding + size;
        }

        m_current->used += total_size;
        return aligned;
    }

    template <typename T, typename ... Args>
    T* emplace(Args&&... args) {
        u8* ptr = alloc_bytes(sizeof(T), alignof(T));
        new (ptr) T(std::forward<Args>(args)...);
        return reinterpret_cast<T*>(ptr);
    }

private:
    static Chunk* allocate_chunk(u64 capacity) {
        Chunk* chunk = static_cast<Chunk*>(malloc(sizeof(Chunk) + capacity));
        chunk->next = nullptr;
        chunk->capacity = capacity;
        chunk->used = 0;
        return chunk;
    }

    Chunk* m_head;     // First chunk in the list
    Chunk* m_current;  // Current chunk being allocated from
};

}

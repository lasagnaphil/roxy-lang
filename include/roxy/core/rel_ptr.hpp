#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"

#include <type_traits>

namespace rx {
template <typename T, u32 Alignment = 1>
struct RelPtr {
    static_assert((Alignment & (Alignment - 1)) == 0, "Alignment must be power of two");

    // TODO: Need separate code for MSVC
    static constexpr u32 AlignmentBits = __builtin_ctz(Alignment);

    // NOTE: We make this type non-copyable and non-moveable,
    //  since this type should only live inside a bump allocator
    //  (unless explicitly copied, in a careful manner)
    RelPtr(const RelPtr& other) = delete;
    RelPtr& operator=(const RelPtr& other) = delete;
    RelPtr(RelPtr&& other) = delete;
    RelPtr& operator=(RelPtr&& other) = delete;

    RelPtr() : m_offset(0) {}
    RelPtr(T* ptr) { set(ptr); }

    RelPtr& operator=(T* ptr) {
        set(ptr);
        return *this;
    }

    T* operator->() { return get(); }
    const T* operator->() const { return get(); }

    T& operator*() { return *get(); }
    const T& operator*() const { return *get(); }

    T* get() {
        return m_offset ? reinterpret_cast<T*>(reinterpret_cast<u8*>(this) + (m_offset << AlignmentBits)) : nullptr;
    }

    const T* get() const {
        return m_offset
                   ? reinterpret_cast<const T*>(reinterpret_cast<const u8*>(this) + (m_offset << AlignmentBits))
                   : nullptr;
    }

    void set(T* ptr) {
        m_offset = (ptr ? (i32)((reinterpret_cast<u8*>(ptr) - reinterpret_cast<u8*>(this)) >> AlignmentBits) : 0);
    }

    bool operator==(nullptr_t) const {
        return m_offset == 0;
    }

    bool operator!=(nullptr_t) const {
        return m_offset != 0;
    }

private:
    i32 m_offset;
};

template <typename T, u32 Alignment = 1>
struct RelSpan {
    // NOTE: We make this type non-copyable and non-moveable,
    //  since this type should only live inside a bump allocator
    //  (unless explicitly copied, in a careful manner)
    RelSpan(const RelSpan& other) = delete;
    RelSpan& operator=(const RelSpan& other) = delete;
    RelSpan(RelSpan&& other) = delete;
    RelSpan& operator=(RelSpan&& other) = delete;

    RelSpan() : m_data(), m_size(0) {}
    RelSpan(T* data, u32 size) : m_data(data), m_size(size) {}
    RelSpan(Span<T> span) : RelSpan(span.data(), span.size()) {}

    const T* data() const { return m_data.get(); }
    T* data() { return m_data.get(); }

    const T* begin() const { return m_data.get(); }
    T* begin() { return m_data.get(); }
    const T* end() const { return m_data.get() + m_size; }
    T* end() { return m_data.get() + m_size; }

    const T& operator[](u32 i) const {
        assert(i < m_size);
        return m_data.get()[i];
    }

    T& operator[](u32 i) {
        assert(i < m_size);
        return m_data.get()[i];
    }

    u32 size() const { return m_size; }
    i32 ssize() const { return (i32)m_size; }

    Span<T> to_span() { return {m_data.get(), m_size}; }

private:
    RelPtr<T, Alignment> m_data;
    u32 m_size;
};
}

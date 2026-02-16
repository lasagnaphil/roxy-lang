#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/format.hpp"

#include <cassert>
#include <cstring>

namespace rx {

/// Fixed-capacity stack-allocated string. Buffer is exactly N bytes;
/// max string length is N-1 (last byte reserved for null terminator).
/// Drop-in replacement for `char buf[N]` with automatic length tracking.
template<u32 N>
class StaticString {
    static_assert(N >= 2, "StaticString capacity must be at least 2");

    char m_buf[N];
    u32 m_size = 0;

public:
    // Constructors

    StaticString() { m_buf[0] = '\0'; }

    StaticString(const char* s) {
        m_size = s ? static_cast<u32>(strlen(s)) : 0;
        assert(m_size <= N - 1);
        if (m_size > 0) memcpy(m_buf, s, m_size);
        m_buf[m_size] = '\0';
    }

    StaticString(const char* s, u32 len) : m_size(len) {
        assert(m_size <= N - 1);
        if (m_size > 0) memcpy(m_buf, s, m_size);
        m_buf[m_size] = '\0';
    }

    StaticString(StringView sv) : m_size(sv.size()) {
        assert(m_size <= N - 1);
        if (m_size > 0) memcpy(m_buf, sv.data(), m_size);
        m_buf[m_size] = '\0';
    }

    // Defaulted copy/move (trivially copyable)
    StaticString(const StaticString&) = default;
    StaticString& operator=(const StaticString&) = default;
    StaticString(StaticString&&) = default;
    StaticString& operator=(StaticString&&) = default;

    // Accessors

    const char* data() const { return m_buf; }
    char* data() { return m_buf; }
    const char* c_str() const { return m_buf; }
    u32 size() const { return m_size; }
    u32 length() const { return m_size; }
    bool empty() const { return m_size == 0; }
    static constexpr u32 capacity() { return N - 1; }

    char operator[](u32 i) const { assert(i < m_size); return m_buf[i]; }
    char& operator[](u32 i) { assert(i < m_size); return m_buf[i]; }
    char front() const { assert(m_size > 0); return m_buf[0]; }
    char back() const { assert(m_size > 0); return m_buf[m_size - 1]; }

    // Iterators

    const char* begin() const { return m_buf; }
    char* begin() { return m_buf; }
    const char* end() const { return m_buf + m_size; }
    char* end() { return m_buf + m_size; }

    // Modifiers

    void push_back(char c) {
        assert(m_size < N - 1);
        m_buf[m_size++] = c;
        m_buf[m_size] = '\0';
    }

    void append(const char* s, u32 len) {
        assert(m_size + len <= N - 1);
        memcpy(m_buf + m_size, s, len);
        m_size += len;
        m_buf[m_size] = '\0';
    }

    void append(StringView sv) { append(sv.data(), sv.size()); }

    void clear() {
        m_size = 0;
        m_buf[0] = '\0';
    }

    void set_size(u32 new_size) {
        assert(new_size <= N - 1);
        m_size = new_size;
        m_buf[m_size] = '\0';
    }

    // Conversion

    operator StringView() const { return StringView(m_buf, m_size); }

    // Comparison

    bool operator==(StringView sv) const {
        return StringView(m_buf, m_size) == sv;
    }
    bool operator!=(StringView sv) const { return !(*this == sv); }

    bool operator==(const char* s) const {
        return StringView(m_buf, m_size) == s;
    }
    bool operator!=(const char* s) const { return !(*this == s); }

    template<u32 M>
    bool operator==(const StaticString<M>& other) const {
        return StringView(m_buf, m_size) == StringView(other.data(), other.size());
    }
    template<u32 M>
    bool operator!=(const StaticString<M>& other) const { return !(*this == other); }

    // Format: clear and write formatted string
    template<typename... Args>
    i32 format(const char* fmt, const Args&... args) {
        i32 n = format_to(m_buf, N, fmt, args...);
        m_size = static_cast<u32>(n) < N ? static_cast<u32>(n) : N - 1;
        return n;
    }
};

/// format_to overload for StaticString: clears and writes formatted string.
template<u32 N, typename... Args>
i32 format_to(StaticString<N>& out, const char* fmt, const Args&... args) {
    return out.format(fmt, args...);
}

} // namespace rx

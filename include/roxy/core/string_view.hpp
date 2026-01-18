#pragma once

#include "roxy/core/types.hpp"

#include <cassert>
#include <cstring>

namespace rx {

class StringView {
    const char* m_data = nullptr;
    u32 m_size = 0;

public:
    StringView() = default;
    StringView(const char* data, u32 size) : m_data(data), m_size(size) {}

    // Construct from null-terminated string
    explicit StringView(const char* str) : m_data(str), m_size(str ? (u32)strlen(str) : 0) {}

    const char* data() const { return m_data; }
    u32 size() const { return m_size; }
    u32 length() const { return m_size; }
    bool empty() const { return m_size == 0; }

    const char* begin() const { return m_data; }
    const char* end() const { return m_data + m_size; }

    char operator[](u32 i) const {
        assert(i < m_size);
        return m_data[i];
    }

    char front() const {
        assert(m_size > 0);
        return m_data[0];
    }

    char back() const {
        assert(m_size > 0);
        return m_data[m_size - 1];
    }

    StringView substr(u32 pos, u32 len = ~0u) const {
        assert(pos <= m_size);
        u32 actual_len = (len > m_size - pos) ? (m_size - pos) : len;
        return StringView(m_data + pos, actual_len);
    }

    bool equals(const StringView& other) const {
        if (m_size != other.m_size) return false;
        return memcmp(m_data, other.m_data, m_size) == 0;
    }

    bool equals(const char* str) const {
        u32 len = (u32)strlen(str);
        if (m_size != len) return false;
        return memcmp(m_data, str, m_size) == 0;
    }

    bool operator==(const StringView& other) const { return equals(other); }
    bool operator!=(const StringView& other) const { return !equals(other); }
    bool operator==(const char* str) const { return equals(str); }
    bool operator!=(const char* str) const { return !equals(str); }
};

// Hash function for StringView (for use in hash maps)
struct StringViewHash {
    u64 operator()(StringView sv) const {
        u64 hash = 14695981039346656037ULL;
        for (u32 i = 0; i < sv.size(); i++) {
            hash ^= static_cast<u64>(sv[i]);
            hash *= 1099511628211ULL;
        }
        return hash;
    }
};

// Equality function for StringView (for use in hash maps)
struct StringViewEqual {
    bool operator()(StringView a, StringView b) const {
        return a == b;
    }
};

}

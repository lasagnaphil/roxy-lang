#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"

#include <cassert>
#include <cstring>

namespace rx {

class String {
public:
    static constexpr u32 npos = ~0u;

private:
    // SSO layout (24 bytes total):
    // Heap mode: char* ptr[8] | u32 size[4] | u32 capacity[4] | pad[7] | tag[1]
    // SSO mode:  char buf[23]                                           | tag[1]
    //
    // Tag byte at offset 23:
    //   bit 7 = 1 means SSO mode, bits 0-6 = remaining capacity (22 - len)
    //   tag == 0 means heap mode
    //
    // SSO capacity: 22 chars + null terminator

    static constexpr u32 SSO_CAP = 22;
    static constexpr u8 SSO_BIT = 0x80;

    struct Heap {
        char* ptr;
        u32 size;
        u32 capacity;
        char pad[7];
        u8 tag;  // 0 = heap
    };

    struct SSO {
        char buf[23];
        u8 tag;  // bit7=1, bits0-6 = remaining capacity
    };

    union {
        Heap m_heap;
        SSO m_sso;
    };

    bool is_sso() const { return (m_sso.tag & SSO_BIT) != 0; }
    u32 sso_size() const { return SSO_CAP - (m_sso.tag & 0x7F); }

    void set_sso_size(u32 len) {
        assert(len <= SSO_CAP);
        m_sso.tag = SSO_BIT | static_cast<u8>(SSO_CAP - len);
        m_sso.buf[len] = '\0';
    }

    void init_sso(const char* s, u32 len);
    void init_heap(const char* s, u32 len);
    void free_heap();
    void grow(u32 min_cap);

public:
    // Constructors
    String();
    String(const char* s);
    String(const char* s, u32 len);
    String(StringView sv);
    String(const String& other);
    String(String&& other) noexcept;
    ~String();

    // Assignment
    String& operator=(const String& other);
    String& operator=(String&& other) noexcept;
    String& operator=(const char* s);
    String& operator=(StringView sv);

    // Accessors
    const char* data() const { return is_sso() ? m_sso.buf : m_heap.ptr; }
    char* data() { return is_sso() ? m_sso.buf : m_heap.ptr; }
    const char* c_str() const { return data(); }
    u32 size() const { return is_sso() ? sso_size() : m_heap.size; }
    u32 length() const { return size(); }
    bool empty() const { return size() == 0; }
    u32 capacity() const { return is_sso() ? SSO_CAP : m_heap.capacity; }

    char operator[](u32 i) const { assert(i < size()); return data()[i]; }
    char& operator[](u32 i) { assert(i < size()); return data()[i]; }
    char front() const { assert(!empty()); return data()[0]; }
    char back() const { assert(!empty()); return data()[size() - 1]; }

    // Iterators
    const char* begin() const { return data(); }
    char* begin() { return data(); }
    const char* end() const { return data() + size(); }
    char* end() { return data() + size(); }

    // Modifiers
    void push_back(char c);
    void append(const char* s, u32 len);
    void append(StringView sv) { append(sv.data(), sv.size()); }
    void clear();
    void reserve(u32 new_cap);
    void resize(u32 new_size, char fill = '\0');

    // Conversion
    operator StringView() const { return StringView(data(), size()); }

    // Comparison
    bool operator==(const String& other) const;
    bool operator!=(const String& other) const { return !(*this == other); }
    bool operator==(StringView sv) const;
    bool operator!=(StringView sv) const { return !(*this == sv); }
    bool operator==(const char* s) const;
    bool operator!=(const char* s) const { return !(*this == s); }

    friend bool operator==(StringView sv, const String& s) { return s == sv; }
    friend bool operator!=(StringView sv, const String& s) { return s != sv; }
    friend bool operator==(const char* cs, const String& s) { return s == cs; }
    friend bool operator!=(const char* cs, const String& s) { return s != cs; }

    // Search
    u32 find(char c, u32 pos = 0) const;
    u32 find(const char* needle, u32 pos = 0) const;

    // Substr
    String substr(u32 pos, u32 len = npos) const;
};

} // namespace rx

// Specialization of std::hash for rx::String so that
// tsl::robin_map<String, ...> works without explicit hash/equal args.
namespace std {
template<>
struct hash<rx::String> {
    size_t operator()(const rx::String& s) const noexcept {
        rx::u64 h = 14695981039346656037ULL;
        for (rx::u32 i = 0; i < s.size(); i++) {
            h ^= static_cast<rx::u64>(s[i]);
            h *= 1099511628211ULL;
        }
        return static_cast<size_t>(h);
    }
};
}

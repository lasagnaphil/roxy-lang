#include "roxy/core/string.hpp"

#include <cstdlib>
#include <cstring>

namespace rx {

// Private helpers

void String::init_sso(const char* s, u32 len) {
    memcpy(m_sso.buf, s, len);
    set_sso_size(len);
}

void String::init_heap(const char* s, u32 len) {
    u32 cap = len < 32 ? 32 : len;
    m_heap.ptr = static_cast<char*>(malloc(cap + 1));
    memcpy(m_heap.ptr, s, len);
    m_heap.ptr[len] = '\0';
    m_heap.size = len;
    m_heap.capacity = cap;
    m_heap.tag = 0;
}

void String::free_heap() {
    if (!is_sso()) {
        free(m_heap.ptr);
    }
}

void String::grow(u32 min_cap) {
    u32 old_size;
    const char* old_data;

    if (is_sso()) {
        old_size = sso_size();
        old_data = m_sso.buf;
    } else {
        if (min_cap <= m_heap.capacity) return;
        old_size = m_heap.size;
        old_data = m_heap.ptr;
    }

    u32 new_cap = min_cap;
    if (!is_sso() && m_heap.capacity * 2 > new_cap) {
        new_cap = m_heap.capacity * 2;
    }
    if (new_cap < 32) new_cap = 32;

    char* new_ptr = static_cast<char*>(malloc(new_cap + 1));
    memcpy(new_ptr, old_data, old_size);
    new_ptr[old_size] = '\0';

    if (!is_sso()) {
        free(m_heap.ptr);
    }

    m_heap.ptr = new_ptr;
    m_heap.size = old_size;
    m_heap.capacity = new_cap;
    m_heap.tag = 0;
}

// Constructors

String::String() {
    m_sso.buf[0] = '\0';
    m_sso.tag = SSO_BIT | SSO_CAP;
}

String::String(const char* s) {
    if (!s || *s == '\0') {
        m_sso.buf[0] = '\0';
        m_sso.tag = SSO_BIT | SSO_CAP;
        return;
    }
    u32 len = static_cast<u32>(strlen(s));
    if (len <= SSO_CAP) {
        init_sso(s, len);
    } else {
        init_heap(s, len);
    }
}

String::String(const char* s, u32 len) {
    if (len == 0) {
        m_sso.buf[0] = '\0';
        m_sso.tag = SSO_BIT | SSO_CAP;
        return;
    }
    if (len <= SSO_CAP) {
        init_sso(s, len);
    } else {
        init_heap(s, len);
    }
}

String::String(StringView sv) : String(sv.data(), sv.size()) {}

String::String(const String& other) {
    if (other.is_sso()) {
        memcpy(this, &other, sizeof(String));
    } else {
        init_heap(other.m_heap.ptr, other.m_heap.size);
    }
}

String::String(String&& other) noexcept {
    memcpy(this, &other, sizeof(String));
    other.m_sso.buf[0] = '\0';
    other.m_sso.tag = SSO_BIT | SSO_CAP;
}

String::~String() {
    free_heap();
}

// Assignment

String& String::operator=(const String& other) {
    if (this == &other) return *this;
    free_heap();
    if (other.is_sso()) {
        memcpy(this, &other, sizeof(String));
    } else {
        init_heap(other.m_heap.ptr, other.m_heap.size);
    }
    return *this;
}

String& String::operator=(String&& other) noexcept {
    if (this == &other) return *this;
    free_heap();
    memcpy(this, &other, sizeof(String));
    other.m_sso.buf[0] = '\0';
    other.m_sso.tag = SSO_BIT | SSO_CAP;
    return *this;
}

String& String::operator=(const char* s) {
    u32 len = s ? static_cast<u32>(strlen(s)) : 0;
    free_heap();
    if (len <= SSO_CAP) {
        if (len > 0) memcpy(m_sso.buf, s, len);
        set_sso_size(len);
    } else {
        init_heap(s, len);
    }
    return *this;
}

String& String::operator=(StringView sv) {
    free_heap();
    if (sv.size() <= SSO_CAP) {
        if (sv.size() > 0) memcpy(m_sso.buf, sv.data(), sv.size());
        set_sso_size(sv.size());
    } else {
        init_heap(sv.data(), sv.size());
    }
    return *this;
}

// Modifiers

void String::push_back(char c) {
    u32 sz = size();
    if (is_sso()) {
        if (sz < SSO_CAP) {
            m_sso.buf[sz] = c;
            set_sso_size(sz + 1);
            return;
        }
        grow(sz + 1);
    } else if (sz >= m_heap.capacity) {
        grow(sz + 1);
    }
    m_heap.ptr[sz] = c;
    m_heap.size = sz + 1;
    m_heap.ptr[sz + 1] = '\0';
}

void String::append(const char* s, u32 len) {
    if (len == 0) return;
    u32 sz = size();
    u32 new_size = sz + len;
    if (is_sso() && new_size <= SSO_CAP) {
        memcpy(m_sso.buf + sz, s, len);
        set_sso_size(new_size);
        return;
    }
    if (is_sso() || new_size > m_heap.capacity) {
        grow(new_size);
    }
    memcpy(m_heap.ptr + sz, s, len);
    m_heap.size = new_size;
    m_heap.ptr[new_size] = '\0';
}

void String::clear() {
    free_heap();
    m_sso.buf[0] = '\0';
    m_sso.tag = SSO_BIT | SSO_CAP;
}

void String::reserve(u32 new_cap) {
    if (new_cap <= capacity()) return;
    grow(new_cap);
}

void String::resize(u32 new_size, char fill) {
    u32 old_size = size();
    if (new_size <= old_size) {
        if (is_sso()) {
            set_sso_size(new_size);
        } else if (new_size <= SSO_CAP) {
            char* old_ptr = m_heap.ptr;
            memcpy(m_sso.buf, old_ptr, new_size);
            set_sso_size(new_size);
            free(old_ptr);
        } else {
            m_heap.ptr[new_size] = '\0';
            m_heap.size = new_size;
        }
        return;
    }
    if (is_sso() && new_size <= SSO_CAP) {
        memset(m_sso.buf + old_size, fill, new_size - old_size);
        set_sso_size(new_size);
        return;
    }
    if (is_sso() || new_size > m_heap.capacity) {
        grow(new_size);
    }
    memset(m_heap.ptr + old_size, fill, new_size - old_size);
    m_heap.size = new_size;
    m_heap.ptr[new_size] = '\0';
}

// Comparison

bool String::operator==(const String& other) const {
    u32 sz = size();
    if (sz != other.size()) return false;
    if (sz == 0) return true;
    return memcmp(data(), other.data(), sz) == 0;
}

bool String::operator==(StringView sv) const {
    u32 sz = size();
    if (sz != sv.size()) return false;
    if (sz == 0) return true;
    return memcmp(data(), sv.data(), sz) == 0;
}

bool String::operator==(const char* s) const {
    if (!s) return empty();
    u32 len = static_cast<u32>(strlen(s));
    u32 sz = size();
    if (sz != len) return false;
    return memcmp(data(), s, sz) == 0;
}

// Search

u32 String::find(char c, u32 pos) const {
    u32 sz = size();
    const char* d = data();
    for (u32 i = pos; i < sz; i++) {
        if (d[i] == c) return i;
    }
    return npos;
}

u32 String::find(const char* needle, u32 pos) const {
    if (!needle || *needle == '\0') return pos <= size() ? pos : npos;
    u32 needle_len = static_cast<u32>(strlen(needle));
    u32 sz = size();
    if (needle_len > sz) return npos;
    const char* d = data();
    for (u32 i = pos; i + needle_len <= sz; i++) {
        if (memcmp(d + i, needle, needle_len) == 0) return i;
    }
    return npos;
}

// Substr

String String::substr(u32 pos, u32 len) const {
    u32 sz = size();
    assert(pos <= sz);
    u32 actual_len = (len > sz - pos) ? (sz - pos) : len;
    return String(data() + pos, actual_len);
}

} // namespace rx

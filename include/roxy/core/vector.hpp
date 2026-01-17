#pragma once

#include "roxy/core/span.hpp"

#include <type_traits>
#include <initializer_list>
#include <iterator>
#include <cstring>
#include <cassert>

namespace rx {

template <typename T, typename Index = u32>
class Vector {
    Index m_capacity, m_size;
    T* m_data = nullptr;

public:
    Vector(Index size = 0) : m_capacity(size), m_size(size), m_data(size ? new T[size] : nullptr) {}

    Vector(Index size, const T& item) {
        for (Index i = 0; i < m_size; i++) {
            m_data[i] = item;
        }
    }

    ~Vector() {
        if (m_data) {
            delete[] m_data;
        }
    }

    Vector(const Vector& other) : m_capacity(other.m_capacity), m_size(other.m_size),
                                  m_data(other.m_capacity ? new T[other.m_capacity] : nullptr)
    {
        copy(other.m_data, other.m_size, m_data);
    }

    friend void swap(Vector& first, Vector& second) noexcept {
        using std::swap;

        swap(first.m_capacity, second.m_capacity);
        swap(first.m_size, second.m_size);
        swap(first.m_data, second.m_data);
    }

    Vector(Vector&& other) noexcept {
        swap(*this, other);
    }

    Vector& operator=(Vector other) {
        swap(*this, other);
        return *this;
    }

    Vector(std::initializer_list<T> lst) : m_capacity(lst.size()), m_size(lst.size()),
                                           m_data(lst.size() ? new T[lst.size()] : nullptr)
    {
        copy(std::data(lst), lst.size(), m_data);
    }

    operator Span<T>() {
        return {m_data, m_size};
    }

    operator Span<const T>() const {
        return {m_data, m_size};
    }

    Index size() const { return m_size; }
    Index ssize() const { return (std::make_signed_t<Index>) m_size; }

    Index capacity() const { return m_capacity; }

    const T* data() const { return m_data; }
    T* data() { return m_data; }

    const T* begin() const { return m_data; }
    T* begin() { return m_data; }

    const T* end() const { return m_data + m_size; }
    T* end() { return m_data + m_size; }

    const T& operator[](Index i) const {
        assert(i < m_size);
        return m_data[i];
    };
    T& operator[](Index i) {
        assert(i < m_size);
        return m_data[i];
    }

    const T& front() const { return m_data[0]; }
    T& front() { return m_data[0]; }

    const T& back() const { return m_data[m_size - 1]; }
    T& back() { return m_data[m_size - 1]; }

    bool empty() const { return m_size == 0; }

    void push_back(T elem) {
        ensure_capacity(m_size + 1);
        m_data[m_size++] = std::move(elem);
    }

    T& push_empty() {
        ensure_capacity(m_size + 1);
        return m_data[m_size++];
    }

    template <class ...Args>
    void emplace_back(Args&&... args) {
        ensure_capacity(m_size + 1);
        new (m_data + m_size++) T(args...);
    }

    T pop_back() {
        return m_data[--m_size];
    }

    void resize(Index new_size) {
        T* new_data = new T[new_size];
        move(m_data, m_size, new_data);
        delete[] m_data;
        m_data = new_data;
        m_capacity = m_size = new_size;
    }

    void reserve(Index new_capacity) {
        if (new_capacity <= m_capacity) return;
        T* new_data = new T[new_capacity];
        move(m_data, m_size, new_data);
        delete[] m_data;
        m_data = new_data;
        m_capacity = new_capacity;
    }

    void clear() {
        if (m_data) {
            delete[] m_data;
            m_data = nullptr;
        }
        m_capacity = m_size = 0;
    }

    T* find(const T& item) {
        for (Index i = 0; i < m_size; i++) {
            if (m_data[i] == item) return &m_data[i];
        }
        return nullptr;
    }

    template <class Predicate>
    T* find_if(Predicate&& predicate) {
        for (Index i = 0; i < m_size; i++) {
            if (predicate(m_data[i])) return &m_data[i];
        }
        return nullptr;
    }

private:
    static void copy(const T* src, Index n, T* dst) {
        if constexpr (std::is_trivially_copyable<T>::value) {
            memcpy(dst, src, sizeof(T) * n);
        }
        else {
            for (Index i = 0; i < n; i++) {
                dst[i] = src[i];
            }
        }
    }

    static void move(T* src, Index n, T* dst) {
        if constexpr (std::is_trivially_copyable<T>::value) {
            memmove(dst, src, sizeof(T) * n);
        }
        else {
            for (Index i = 0; i < n; i++) {
                dst[i] = std::move(src[i]);
            }
        }
    }

    void ensure_capacity(Index min_capacity) {
#define MAX(a, b) (((a)>(b))? (a):(b))
        if (min_capacity <= m_capacity) return;
        size_t new_capacity = MAX(m_capacity * 2, min_capacity);
        T* new_data = new T[new_capacity];
        move(m_data, m_size, new_data);
        delete[] m_data;
        m_data = new_data;
        m_capacity = new_capacity;
#undef MAX
    }
};

}
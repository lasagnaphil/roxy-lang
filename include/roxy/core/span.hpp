#pragma once

#include "roxy/core/types.hpp"

namespace rx {

template <typename T, typename Index = u32>
class Span {
    T* m_data;
    Index m_size;

public:
    Span(T* data = nullptr, Index size = 0) : m_data(data), m_size(size) {}

    const T* data() const { return m_data; }
    T* data() { return m_data; }

    const T* begin() const { return m_data; }
    T* begin() { return m_data; }
    const T* end() const { return m_data + m_size; }
    T* end() { return m_data + m_size; }

    const T& operator[](Index i) const {
        assert(i < m_size);
        return m_data[i];
    }
    T& operator[](Index i) {
        assert(i < m_size);
        return m_data[i];
    }

    Index size() const { return m_size; }
    Index ssize() const { return (std::make_signed_t<Index>) m_size; }
};

}
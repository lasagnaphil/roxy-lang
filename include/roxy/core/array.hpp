#pragma once

#include "roxy/core/span.hpp"

#include <cassert>

namespace rx {

template <class T, uint32_t _Size>
class Array {
public:
    T m_data[_Size];

    constexpr uint32_t size() const { return _Size; }

    const T* data() const { return m_data; }
    T* data() { return m_data; }

    const T* begin() const { return m_data; }
    T* begin() { return m_data; }

    const T* end() const { return m_data + _Size; }
    T* end() { return m_data + _Size; }

    constexpr const T& operator[](uint32_t i) const { 
        assert(i < _Size);
        return m_data[i];
    };
    constexpr T& operator[](uint32_t i) { 
        assert(i < _Size);
        return m_data[i];
    };

    constexpr operator Span<T>() { return {m_data, _Size}; }
};

// Template deduction rules for Array. 
// A really obscure part of C++ that almost no one really knows...
template <class T, class... U>
Array(T, U...) -> Array<T, 1 + sizeof...(U)>;

}
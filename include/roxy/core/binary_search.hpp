#pragma once

#include "roxy/core/types.hpp"

namespace rx {

// Optimized branchless binary search from https://en.algorithmica.org/hpc/data-structures/binary-search/.

template <typename T, typename Index>
Index binary_search(const T* arr, Index n, T x) {
    const T* base = arr;
    Index len = n;
    while (len > 1) {
        Index half = len / 2;
        len -= half;
        __builtin_prefetch(&base[len / 2 - 1]);
        __builtin_prefetch(&base[half + len / 2 - 1]);
        base += (base[half - 1] < x) * half;
    }
    return base - arr;
}

}

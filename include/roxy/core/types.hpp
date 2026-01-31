#pragma once

#include <stdint.h>

// Force inline macro for performance-critical code
#if defined(_MSC_VER)
    #define RX_FORCEINLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
    #define RX_FORCEINLINE __attribute__((always_inline)) inline
#else
    #define RX_FORCEINLINE inline
#endif

namespace rx {

using i8 = int8_t;
using u8 = uint8_t;
using i16 = int16_t;
using u16 = uint16_t;
using i32 = int32_t;
using u32 = uint32_t;
using i64 = int64_t;
using u64 = uint64_t;
using f32 = float;
using f64 = double;

}
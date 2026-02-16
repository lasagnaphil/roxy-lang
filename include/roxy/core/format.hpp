#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/string.hpp"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace rx {
namespace detail {

// Buffer write helpers. Callers guarantee size >= 1.
inline void fmt_write(char* buf, u32 size, u32& pos, const char* s, u32 len) {
    for (u32 i = 0; i < len; i++) {
        if (pos < size - 1) buf[pos] = s[i];
        pos++;
    }
}

inline void fmt_write_char(char* buf, u32 size, u32& pos, char c) {
    if (pos < size - 1) buf[pos] = c;
    pos++;
}

// Type-erased format argument
using FmtWriteFn = void(*)(char*, u32, u32&, const void*);

struct FmtArg {
    const void* ptr;
    FmtWriteFn write;
};

// Value formatters

inline void fmt_bool(char* buf, u32 size, u32& pos, const void* val) {
    const char* s = *static_cast<const bool*>(val) ? "true" : "false";
    fmt_write(buf, size, pos, s, static_cast<u32>(strlen(s)));
}

inline void fmt_char(char* buf, u32 size, u32& pos, const void* val) {
    fmt_write_char(buf, size, pos, *static_cast<const char*>(val));
}

template<typename T>
void fmt_signed(char* buf, u32 size, u32& pos, const void* val) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%lld",
                     static_cast<long long>(*static_cast<const T*>(val)));
    if (n > 0) fmt_write(buf, size, pos, tmp, static_cast<u32>(n));
}

template<typename T>
void fmt_unsigned(char* buf, u32 size, u32& pos, const void* val) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%llu",
                     static_cast<unsigned long long>(*static_cast<const T*>(val)));
    if (n > 0) fmt_write(buf, size, pos, tmp, static_cast<u32>(n));
}

inline void fmt_f32(char* buf, u32 size, u32& pos, const void* val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%g",
                     static_cast<double>(*static_cast<const f32*>(val)));
    if (n > 0) fmt_write(buf, size, pos, tmp, static_cast<u32>(n));
}

inline void fmt_f64(char* buf, u32 size, u32& pos, const void* val) {
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "%g", *static_cast<const f64*>(val));
    if (n > 0) fmt_write(buf, size, pos, tmp, static_cast<u32>(n));
}

inline void fmt_cstr(char* buf, u32 size, u32& pos, const void* val) {
    const char* s = static_cast<const char*>(val);
    if (s) fmt_write(buf, size, pos, s, static_cast<u32>(strlen(s)));
    else fmt_write(buf, size, pos, "(null)", 6);
}

inline void fmt_sv(char* buf, u32 size, u32& pos, const void* val) {
    const auto* sv = static_cast<const StringView*>(val);
    fmt_write(buf, size, pos, sv->data(), sv->size());
}

inline void fmt_string(char* buf, u32 size, u32& pos, const void* val) {
    const auto* s = static_cast<const String*>(val);
    fmt_write(buf, size, pos, s->data(), s->size());
}

inline void fmt_ptr(char* buf, u32 size, u32& pos, const void* val) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%p", val);
    if (n > 0) fmt_write(buf, size, pos, tmp, static_cast<u32>(n));
}

// Type dispatch via constexpr if

template<typename T>
FmtArg make_arg(const T& val) {
    using D = std::decay_t<T>;

    if constexpr (std::is_same_v<D, bool>) {
        return {&val, fmt_bool};
    } else if constexpr (std::is_same_v<D, char>) {
        return {&val, fmt_char};
    } else if constexpr (std::is_same_v<D, const char*> || std::is_same_v<D, char*>) {
        // Store the string pointer directly (handles string literals and char* vars)
        return {static_cast<const void*>(static_cast<const char*>(val)), fmt_cstr};
    } else if constexpr (std::is_same_v<D, StringView>) {
        return {&val, fmt_sv};
    } else if constexpr (std::is_same_v<D, String>) {
        return {&val, fmt_string};
    } else if constexpr (std::is_enum_v<D>) {
        using U = std::underlying_type_t<D>;
        if constexpr (std::is_signed_v<U>) {
            return {&val, fmt_signed<D>};
        } else {
            return {&val, fmt_unsigned<D>};
        }
    } else if constexpr (std::is_integral_v<D> && std::is_signed_v<D>) {
        return {&val, fmt_signed<D>};
    } else if constexpr (std::is_integral_v<D> && std::is_unsigned_v<D>) {
        return {&val, fmt_unsigned<D>};
    } else if constexpr (std::is_same_v<D, f32>) {
        return {&val, fmt_f32};
    } else if constexpr (std::is_same_v<D, f64>) {
        return {&val, fmt_f64};
    } else if constexpr (std::is_pointer_v<D>) {
        // Store the pointer value directly for address formatting
        return {static_cast<const void*>(val), fmt_ptr};
    } else {
        static_assert(!sizeof(D*), "Unsupported type for rx::format_to");
    }
}

// Core format string parser (non-template)

inline i32 fmt_impl(char* buf, u32 size, const char* fmt, const FmtArg* args, u32 nargs) {
    if (size == 0) return 0;

    u32 pos = 0;
    u32 ai = 0;

    while (*fmt) {
        if (fmt[0] == '{') {
            if (fmt[1] == '{') {
                // Escaped brace: {{ -> {
                fmt_write_char(buf, size, pos, '{');
                fmt += 2;
            } else if (fmt[1] == '}') {
                // Placeholder: {}
                if (ai < nargs) {
                    args[ai].write(buf, size, pos, args[ai].ptr);
                    ai++;
                }
                fmt += 2;
            } else {
                // Lone { with no matching } - write literally
                fmt_write_char(buf, size, pos, *fmt);
                fmt++;
            }
        } else if (fmt[0] == '}' && fmt[1] == '}') {
            // Escaped brace: }} -> }
            fmt_write_char(buf, size, pos, '}');
            fmt += 2;
        } else {
            fmt_write_char(buf, size, pos, *fmt);
            fmt++;
        }
    }

    // Always null-terminate
    buf[pos < size ? pos : size - 1] = '\0';
    return static_cast<i32>(pos);
}

} // namespace detail

/// Format to a buffer using {} placeholders.
/// Returns number of chars that would have been written (excluding null terminator).
/// If return value >= size, output was truncated. Always null-terminates if size > 0.
/// Use {{ and }} to write literal braces.
/// Supports: bool, char, integers, f32, f64, const char*, StringView, pointers, enums.
template<typename... Args>
i32 format_to(char* buf, u32 size, const char* fmt, const Args&... args) {
    if constexpr (sizeof...(Args) == 0) {
        return detail::fmt_impl(buf, size, fmt, nullptr, 0);
    } else {
        detail::FmtArg arg_array[] = {detail::make_arg(args)...};
        return detail::fmt_impl(buf, size, fmt, arg_array, sizeof...(Args));
    }
}

/// Format and return a String using {} placeholders.
template<typename... Args>
String format(const char* fmt, const Args&... args) {
    char stack_buf[256];
    i32 n = format_to(stack_buf, sizeof(stack_buf), fmt, args...);
    if (static_cast<u32>(n) < sizeof(stack_buf)) {
        return String(stack_buf, static_cast<u32>(n));
    }
    String result;
    result.resize(static_cast<u32>(n));
    format_to(result.data(), static_cast<u32>(n) + 1, fmt, args...);
    return result;
}

} // namespace rx

#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/string.hpp"

#include <cstdio>
#include <cstring>
#include <type_traits>

namespace rx {

// Forward declaration for StaticString support
template<u32 N> class StaticString;

template<typename T> struct is_static_string : std::false_type {};
template<u32 N> struct is_static_string<StaticString<N>> : std::true_type {};

namespace detail {

// Format spec: {:[fill][align][sign][0][width][type]}
struct FmtSpec {
    char fill = ' ';
    char align = 0;   // 0 = default ('>'), '<' = left, '>' = right
    char sign = '-';   // '-' = only negative, '+' = always
    u32 width = 0;
    char type = 0;     // 'x' = hex lower, 'X' = hex upper, 0 = default
};

inline FmtSpec parse_format_spec(const char* start, const char* end) {
    FmtSpec spec;
    const char* p = start;

    // Check for [fill]align
    if (p + 1 < end && (p[1] == '<' || p[1] == '>')) {
        spec.fill = p[0];
        spec.align = p[1];
        p += 2;
    } else if (p < end && (*p == '<' || *p == '>')) {
        spec.align = *p;
        p++;
    }

    // Check for sign
    if (p < end && (*p == '+' || *p == '-')) {
        spec.sign = *p;
        p++;
    }

    // Check for 0-fill (before width)
    if (p < end && *p == '0') {
        if (!spec.align) {
            spec.fill = '0';
            spec.align = '>';
        }
        p++;
    }

    // Parse width
    while (p < end && *p >= '0' && *p <= '9') {
        spec.width = spec.width * 10 + static_cast<u32>(*p - '0');
        p++;
    }

    // Parse type
    if (p < end && (*p == 'x' || *p == 'X')) {
        spec.type = *p;
        p++;
    }

    return spec;
}

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

// Apply format spec (padding/alignment) to a formatted value
inline void fmt_apply_spec(char* buf, u32 size, u32& pos, const char* val, u32 val_len, const FmtSpec* spec) {
    if (!spec || spec->width <= val_len) {
        fmt_write(buf, size, pos, val, val_len);
        return;
    }
    u32 pad = spec->width - val_len;
    char fill = spec->fill;
    char align = spec->align ? spec->align : '>';

    if (fill == '0' && align == '>' && val_len > 0 && (val[0] == '+' || val[0] == '-')) {
        // Zero-padding with sign: sign first, then zeros, then digits
        fmt_write_char(buf, size, pos, val[0]);
        for (u32 i = 0; i < pad; i++) fmt_write_char(buf, size, pos, '0');
        fmt_write(buf, size, pos, val + 1, val_len - 1);
    } else if (align == '<') {
        fmt_write(buf, size, pos, val, val_len);
        for (u32 i = 0; i < pad; i++) fmt_write_char(buf, size, pos, fill);
    } else {
        for (u32 i = 0; i < pad; i++) fmt_write_char(buf, size, pos, fill);
        fmt_write(buf, size, pos, val, val_len);
    }
}

// Type-erased format argument
using FmtWriteFn = void(*)(char*, u32, u32&, const void*, const FmtSpec*);

struct FmtArg {
    const void* ptr;
    FmtWriteFn write;
};

// Value formatters

inline void fmt_bool(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    const char* s = *static_cast<const bool*>(val) ? "true" : "false";
    u32 len = static_cast<u32>(strlen(s));
    fmt_apply_spec(buf, size, pos, s, len, spec);
}

inline void fmt_char(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char c = *static_cast<const char*>(val);
    fmt_apply_spec(buf, size, pos, &c, 1, spec);
}

template<typename T>
void fmt_signed(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char tmp[24];
    int n;
    long long ll = static_cast<long long>(*static_cast<const T*>(val));
    if (spec && (spec->type == 'x' || spec->type == 'X')) {
        const char* hex_fmt = spec->type == 'x' ? "%llx" : "%llX";
        n = snprintf(tmp, sizeof(tmp), hex_fmt, static_cast<unsigned long long>(ll));
    } else if (spec && spec->sign == '+' && ll >= 0) {
        n = snprintf(tmp, sizeof(tmp), "+%lld", ll);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%lld", ll);
    }
    if (n > 0) fmt_apply_spec(buf, size, pos, tmp, static_cast<u32>(n), spec);
}

template<typename T>
void fmt_unsigned(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char tmp[24];
    int n;
    unsigned long long ull = static_cast<unsigned long long>(*static_cast<const T*>(val));
    if (spec && (spec->type == 'x' || spec->type == 'X')) {
        const char* hex_fmt = spec->type == 'x' ? "%llx" : "%llX";
        n = snprintf(tmp, sizeof(tmp), hex_fmt, ull);
    } else if (spec && spec->sign == '+') {
        n = snprintf(tmp, sizeof(tmp), "+%llu", ull);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%llu", ull);
    }
    if (n > 0) fmt_apply_spec(buf, size, pos, tmp, static_cast<u32>(n), spec);
}

inline void fmt_f32(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char tmp[32];
    double v = static_cast<double>(*static_cast<const f32*>(val));
    int n;
    if (spec && spec->sign == '+' && v >= 0) {
        n = snprintf(tmp, sizeof(tmp), "+%g", v);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%g", v);
    }
    if (n > 0) fmt_apply_spec(buf, size, pos, tmp, static_cast<u32>(n), spec);
}

inline void fmt_f64(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char tmp[32];
    double v = *static_cast<const f64*>(val);
    int n;
    if (spec && spec->sign == '+' && v >= 0) {
        n = snprintf(tmp, sizeof(tmp), "+%g", v);
    } else {
        n = snprintf(tmp, sizeof(tmp), "%g", v);
    }
    if (n > 0) fmt_apply_spec(buf, size, pos, tmp, static_cast<u32>(n), spec);
}

inline void fmt_cstr(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    const char* s = static_cast<const char*>(val);
    if (s) {
        u32 len = static_cast<u32>(strlen(s));
        fmt_apply_spec(buf, size, pos, s, len, spec);
    } else {
        fmt_apply_spec(buf, size, pos, "(null)", 6, spec);
    }
}

inline void fmt_sv(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    const auto* sv = static_cast<const StringView*>(val);
    fmt_apply_spec(buf, size, pos, sv->data(), sv->size(), spec);
}

inline void fmt_string(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    const auto* s = static_cast<const String*>(val);
    fmt_apply_spec(buf, size, pos, s->data(), s->size(), spec);
}

inline void fmt_ptr(char* buf, u32 size, u32& pos, const void* val, const FmtSpec* spec) {
    char tmp[24];
    int n = snprintf(tmp, sizeof(tmp), "%p", val);
    if (n > 0) fmt_apply_spec(buf, size, pos, tmp, static_cast<u32>(n), spec);
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
    } else if constexpr (is_static_string<D>::value) {
        // StaticString<N>: first member is char m_buf[N], so &val points to the buffer.
        // The string is always null-terminated, so fmt_cstr works directly.
        return {static_cast<const void*>(&val), fmt_cstr};
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
                    args[ai].write(buf, size, pos, args[ai].ptr, nullptr);
                    ai++;
                }
                fmt += 2;
            } else if (fmt[1] == ':') {
                // Format spec: {:spec}
                const char* spec_start = fmt + 2;
                const char* spec_end = spec_start;
                while (*spec_end && *spec_end != '}') spec_end++;
                if (*spec_end == '}') {
                    FmtSpec spec = parse_format_spec(spec_start, spec_end);
                    if (ai < nargs) {
                        args[ai].write(buf, size, pos, args[ai].ptr, &spec);
                        ai++;
                    }
                    fmt = spec_end + 1;
                } else {
                    // No closing }, write literally
                    fmt_write_char(buf, size, pos, *fmt);
                    fmt++;
                }
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
/// Supports format specifiers: {:04} {:+} {:<12} {:>8} {:08x} {:08X}
/// Supports: bool, char, integers, f32, f64, const char*, StringView, String, StaticString, pointers, enums.
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

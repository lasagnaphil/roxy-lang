#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/shared/token_kinds.hpp"

namespace rx {

struct SourceLocation {
    u32 offset;       // Byte offset in source (start)
    u32 end_offset;   // Byte offset in source (end, for LSP diagnostic ranges)
    u32 line;         // 1-indexed line number
    u32 column;       // 1-indexed column number
};

struct Token {
    TokenKind kind;
    SourceLocation loc;
    const char* start;  // Pointer into source buffer
    u32 length;

    // For numeric literals, store parsed value directly
    union {
        i64 int_value;
        f64 float_value;
    };

    // Helper to get string view of token text
    StringView text() const { return {start, length}; }
};

}

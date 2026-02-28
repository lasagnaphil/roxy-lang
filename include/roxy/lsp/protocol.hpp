#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/lsp/syntax_tree.hpp"

namespace rx {

// LSP position (0-indexed line and character)
struct LspPosition {
    u32 line;
    u32 character;
};

// LSP range
struct LspRange {
    LspPosition start;
    LspPosition end;
};

// LSP diagnostic severity
enum class DiagnosticSeverity : u8 {
    Error = 1,
    Warning = 2,
    Information = 3,
    Hint = 4,
};

// LSP diagnostic
struct LspDiagnostic {
    LspRange range;
    DiagnosticSeverity severity;
    String message;
};

// Convert a byte offset in source text to an LSP position (0-indexed line/character)
inline LspPosition offset_to_lsp_position(const char* source, u32 length, u32 byte_offset) {
    u32 line = 0;
    u32 line_start = 0;

    for (u32 i = 0; i < byte_offset && i < length; i++) {
        if (source[i] == '\n') {
            line++;
            line_start = i + 1;
        }
    }

    return LspPosition{line, byte_offset - line_start};
}

// Convert a TextRange to an LspRange
inline LspRange text_range_to_lsp_range(const char* source, u32 length, TextRange range) {
    return LspRange{
        offset_to_lsp_position(source, length, range.start),
        offset_to_lsp_position(source, length, range.end),
    };
}

// Convert an LspPosition to a byte offset in source text
inline u32 lsp_position_to_offset(const char* source, u32 length, LspPosition pos) {
    u32 line = 0;
    u32 offset = 0;
    while (offset < length && line < pos.line) {
        if (source[offset] == '\n') line++;
        offset++;
    }
    // Now at start of target line, advance by character count
    offset += pos.character;
    if (offset > length) offset = length;
    return offset;
}

// LSP CompletionItemKind constants
namespace CompletionItemKind {
    constexpr i64 Method = 2;
    constexpr i64 Function = 3;
    constexpr i64 Constructor = 4;
    constexpr i64 Field = 5;
    constexpr i64 Variable = 6;
    constexpr i64 Class = 7;       // for structs
    constexpr i64 Interface = 8;   // for traits
    constexpr i64 Enum = 13;
    constexpr i64 Keyword = 14;
    constexpr i64 EnumMember = 20;
    constexpr i64 Struct = 22;
}

} // namespace rx

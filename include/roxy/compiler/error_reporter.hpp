#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
#include "roxy/shared/token.hpp"

#include <cstring>

namespace rx {

// Semantic error with location information
struct SemanticError {
    SourceLocation loc;
    const char* message;

    // For formatted messages, we store them in the allocator
    bool owns_message;
};

// Maximum number of errors to collect before stopping
constexpr u32 MAX_SEMANTIC_ERRORS = 20;
constexpr u32 MAX_LSP_SEMANTIC_ERRORS = 200;

// Collects semantic errors with a cap (raised in LSP mode) and printf-like
// formatting into a bump allocator. Held by the semantic analyzer and shared
// (by reference) with its collaborators, so any of them can report errors
// without holding a back-reference to the analyzer itself.
class ErrorReporter {
public:
    explicit ErrorReporter(BumpAllocator& allocator) : m_allocator(allocator) {}

    void set_lsp_mode(bool enable) { m_lsp_mode = enable; }
    bool lsp_mode() const { return m_lsp_mode; }

    bool has_errors() const { return !m_errors.empty(); }
    const Vector<SemanticError>& errors() const { return m_errors; }

    bool too_many_errors() const {
        u32 limit = m_lsp_mode ? MAX_LSP_SEMANTIC_ERRORS : MAX_SEMANTIC_ERRORS;
        return m_errors.size() >= limit;
    }

    void error(SourceLocation loc, const char* message) {
        if (too_many_errors()) return;
        m_errors.push_back({loc, message, false});
    }

    template<typename... Args>
    void error_fmt(SourceLocation loc, const char* fmt, const Args&... args) {
        if (too_many_errors()) return;

        char buffer[512];
        format_to(buffer, sizeof(buffer), fmt, args...);

        u32 len = static_cast<u32>(strlen(buffer));
        char* msg = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
        memcpy(msg, buffer, len + 1);

        m_errors.push_back({loc, msg, true});
    }

private:
    BumpAllocator& m_allocator;
    Vector<SemanticError> m_errors;
    bool m_lsp_mode = false;
};

}

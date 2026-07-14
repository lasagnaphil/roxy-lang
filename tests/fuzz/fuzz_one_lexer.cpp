#include "fuzz_targets.hpp"

#include "roxy/shared/lexer.hpp"

namespace rx::fuzz {

void fuzz_one_lexer(const uint8_t* data, size_t size) {
    detail::SourceBuffer src(data, size);
    if (!src.ok) return;

    Lexer lexer(src.data(), src.length());
    // Drain to EOF. A non-advancing loop here would manifest as a libFuzzer
    // timeout (hang), which is the intended signal for lexer non-termination.
    while (true) {
        Token token = lexer.next_token();
        if (token.kind == TokenKind::Eof) break;
    }
}

} // namespace rx::fuzz

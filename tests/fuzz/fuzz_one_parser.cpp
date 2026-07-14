#include "fuzz_targets.hpp"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"

namespace rx::fuzz {

void fuzz_one_parser(const uint8_t* data, size_t size) {
    detail::SourceBuffer src(data, size);
    if (!src.ok) return;

    Lexer lexer(src.data(), src.length());
    // Fresh allocator per input — all AST nodes live here and are freed when it
    // is destroyed at scope exit, so there is no cross-input state leakage.
    BumpAllocator allocator(4096);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();
    (void)program;  // Fail-fast parser stops at the first error; we only care
                    // that it never crashes.
}

} // namespace rx::fuzz

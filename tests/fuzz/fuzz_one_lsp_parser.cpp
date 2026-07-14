#include "fuzz_targets.hpp"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/lsp/lsp_parser.hpp"

namespace rx::fuzz {

void fuzz_one_lsp_parser(const uint8_t* data, size_t size) {
    detail::SourceBuffer src(data, size);
    if (!src.ok) return;

    Lexer lexer(src.data(), src.length());
    BumpAllocator allocator(4096);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();
    (void)tree;  // Error-recovering parser must always produce a tree, never
                 // crash, on any input.
}

} // namespace rx::fuzz

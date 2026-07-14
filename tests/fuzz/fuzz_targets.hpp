#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// Shared fuzz-target bodies for the lexer, parser, and LSP error-recovering
// parser. Each takes a raw, NON-null-terminated byte buffer (the libFuzzer
// contract) and drives one component to completion, exercising it for crashes,
// assertion failures, hangs, and (under ASAN/UBSan) memory/UB errors.
//
// One body per component lives in its own TU (fuzz_one_lexer.cpp, ...) so each
// fuzz executable links only the library it exercises:
//   - tests/fuzz/fuzz_lexer.cpp / fuzz_parser.cpp / fuzz_lsp_parser.cpp wrap
//     each in an `LLVMFuzzerTestOneInput` entry point (the fuzz executables).
//   - tests/unit/test_fuzz_regression.cpp replays the seed corpus + saved crash
//     inputs through all three inside the normal doctest suite, so found-and-
//     fixed crashes stay fixed and the harnesses never bit-rot.
namespace rx::fuzz {

// Tokenize the input to EOF. Reveals lexer non-termination, over-reads past the
// length-bounded buffer (the unchecked `advance()`), and number/escape UB.
void fuzz_one_lexer(const uint8_t* data, size_t size);

// Run the fail-fast recursive-descent parser over the input. Reveals parser
// crashes and stack overflow on pathologically nested input.
void fuzz_one_parser(const uint8_t* data, size_t size);

// Run the LSP error-recovering parser over the input. Highest-value target:
// it is explicitly designed to consume arbitrary/malformed input and must
// never crash regardless of how broken the input is.
void fuzz_one_lsp_parser(const uint8_t* data, size_t size);

namespace detail {

// An exact-size, NON-null-terminated heap copy of the fuzz input.
//
// The Lexer takes a (const char*, u32) length. libFuzzer inputs are neither
// null-terminated nor bounded to u32, so we:
//   - Reject > UINT32_MAX inputs (the lexer's offsets are u32; production
//     sources are far smaller and size is not the property under test).
//   - Copy into an exact-size heap buffer with NO trailing '\0'. This is
//     deliberately adversarial: production always null-terminates its source,
//     so any read past `size` here is a real out-of-bounds access (the lexer's
//     unchecked `advance()`), which a fresh exact-size allocation makes far
//     more likely to fault / be caught by ASAN than reading into libFuzzer's
//     persistent, padded input buffer.
struct SourceBuffer {
    std::vector<char> bytes;
    bool ok;

    SourceBuffer(const uint8_t* data, size_t size)
        : ok(size <= static_cast<size_t>(UINT32_MAX)) {
        if (ok) {
            bytes.assign(reinterpret_cast<const char*>(data),
                         reinterpret_cast<const char*>(data) + size);
        }
    }

    const char* data() const { return bytes.data(); }
    uint32_t length() const { return static_cast<uint32_t>(bytes.size()); }
};

} // namespace detail
} // namespace rx::fuzz

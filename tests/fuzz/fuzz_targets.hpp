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

// An exact-(size+1) heap copy of the fuzz input with a single trailing '\0'.
//
// The Lexer takes a (const char*, u32) length and relies on a NUL sentinel at
// source[length] (peek() is an unchecked load — see OPTIMIZATION.md §3.2).
// libFuzzer inputs are neither null-terminated nor bounded to u32, so we:
//   - Reject > UINT32_MAX inputs (the lexer's offsets are u32; production
//     sources are far smaller and size is not the property under test).
//   - Copy into a fresh heap buffer of exactly `size + 1` bytes and write the
//     terminator at [size], matching production's contract (read_file et al.
//     all allocate length+1). length() still reports the logical `size`.
// This stays adversarial: the allocation is exactly size+1, so any read *past*
// the sentinel — the class of bug the sentinel could now mask — lands one byte
// out of bounds and is caught by ASAN, rather than reading into libFuzzer's
// persistent, padded input buffer.
struct SourceBuffer {
    std::vector<char> bytes;  // `len` logical bytes + 1 trailing '\0'
    uint32_t len = 0;
    bool ok;

    SourceBuffer(const uint8_t* data, size_t size)
        : ok(size <= static_cast<size_t>(UINT32_MAX)) {
        if (ok) {
            len = static_cast<uint32_t>(size);
            bytes.reserve(size + 1);
            bytes.assign(reinterpret_cast<const char*>(data),
                         reinterpret_cast<const char*>(data) + size);
            bytes.push_back('\0');
        }
    }

    const char* data() const { return bytes.data(); }
    uint32_t length() const { return len; }
};

} // namespace detail
} // namespace rx::fuzz

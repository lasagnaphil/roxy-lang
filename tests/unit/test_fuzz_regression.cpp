// Replays the fuzz seed corpus, the example programs, and a set of inline
// adversarial inputs through the three fuzz harnesses (lexer / parser / LSP
// parser) inside the normal doctest run. This keeps the harnesses compiling and
// linkable, exercises the components against known-tricky input without needing
// a fuzzer-capable toolchain, and — because saved crash reproductions live in
// tests/fuzz/corpus/ — locks in every crash the fuzzer finds and we fix.
//
// A genuine crash here aborts the test process (which doctest reports as a
// failure); reaching the end of a case means every input survived all three
// harnesses.
#include "roxy/core/doctest/doctest.h"

#include "../fuzz/fuzz_targets.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace rx;

namespace {

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(file),
                                std::istreambuf_iterator<char>());
}

// Run one input through all three harnesses. `data` may be null iff `size` is 0.
void run_all_harnesses(const uint8_t* data, size_t size) {
    fuzz::fuzz_one_lexer(data, size);
    fuzz::fuzz_one_parser(data, size);
    fuzz::fuzz_one_lsp_parser(data, size);
}

void run_all_harnesses(const std::vector<uint8_t>& bytes) {
    run_all_harnesses(bytes.data(), bytes.size());
}

// Replay every regular file directly under `dir` (recursively) through the
// harnesses. Returns the number of files replayed.
int replay_directory(const fs::path& dir, const char* extension_filter) {
    if (!fs::exists(dir)) return 0;
    int count = 0;
    for (const auto& entry : fs::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (extension_filter && entry.path().extension() != extension_filter) continue;
        std::vector<uint8_t> bytes = read_bytes(entry.path());
        INFO("replaying " << entry.path().string() << " (" << bytes.size() << " bytes)");
        run_all_harnesses(bytes);
        count++;
    }
    return count;
}

#ifdef ROXY_PROJECT_ROOT
const fs::path g_project_root = ROXY_PROJECT_ROOT;
#else
const fs::path g_project_root = ".";
#endif

} // namespace

TEST_SUITE("Fuzz Regression") {

    // Saved crash reproductions + hand-written edge cases. When the fuzzer finds
    // a crash, drop the reproducer file here and it becomes a permanent
    // regression test.
    TEST_CASE("seed corpus survives all harnesses") {
        int replayed = replay_directory(g_project_root / "tests" / "fuzz" / "corpus", nullptr);
        CHECK(replayed > 0);  // guard against a silently-empty corpus path
    }

    // The example programs are large, valid inputs — happy-path coverage that a
    // pure edge-case corpus misses.
    TEST_CASE("example programs survive all harnesses") {
        int replayed = replay_directory(g_project_root / "examples", ".roxy");
        CHECK(replayed > 0);
    }

    // Inline adversarial inputs, independent of any on-disk file. These stress
    // the lexer/parser boundaries that valid programs never reach.
    TEST_CASE("adversarial inline inputs survive all harnesses") {
        // Empty input (data may be null, size 0).
        run_all_harnesses(nullptr, 0);

        // Single interesting bytes and short token prefixes.
        const char* snippets[] = {
            "\"", "'", "{", "}", "(", ")", "[", "]", "<", ">", "\\", ".", ",",
            ";", ":", "?", "~", "!", "=", "+", "-", "*", "/", "%", "&", "|", "^",
            "//", "/*", "*/", "0x", "0b", "0o", "1e", "1.", ".1", "1_", "0x_",
            "f\"", "f\"{", "f\"}", "f\"{}\"", "\"\\", "'\\'", "<<", ">>", "<=",
            "==", "->", "::", "..", "@#$", "\t\r\n", "     ",
        };
        for (const char* s : snippets) {
            INFO("snippet: " << s);
            run_all_harnesses(reinterpret_cast<const uint8_t*>(s), std::string(s).size());
        }

        // Embedded NUL bytes must not terminate scanning early or over-read.
        {
            const uint8_t with_nul[] = {'v', 'a', 'r', ' ', 'x', 0, '=', '1', ';'};
            run_all_harnesses(with_nul, sizeof(with_nul));
        }

        // Raw non-UTF-8 / high bytes.
        {
            std::vector<uint8_t> high_bytes;
            for (int b = 0x80; b <= 0xFF; b++) high_bytes.push_back((uint8_t)b);
            run_all_harnesses(high_bytes);
        }

        // Bounded deep nesting — descends the recursive-descent parser ~500
        // levels (well within the native stack). Unbounded nesting is left to
        // the fuzzer proper under a `-max_len` cap; see tests/fuzz/README.md.
        {
            std::string deep(500, '(');
            deep += "1";
            run_all_harnesses(reinterpret_cast<const uint8_t*>(deep.data()), deep.size());
        }

        // Long run of a single character (buffer-boundary / no-terminator stress).
        {
            std::vector<uint8_t> long_run(4096, (uint8_t)'*');
            run_all_harnesses(long_run);
        }

        // Unterminated constructs at exact end-of-buffer (no trailing NUL).
        {
            std::string unterminated_str = "var s = \"abc";
            run_all_harnesses(reinterpret_cast<const uint8_t*>(unterminated_str.data()),
                              unterminated_str.size());
            std::string unterminated_comment = "/* /* nested";
            run_all_harnesses(reinterpret_cast<const uint8_t*>(unterminated_comment.data()),
                              unterminated_comment.size());
            std::string unterminated_fstr = "f\"x {y";
            run_all_harnesses(reinterpret_cast<const uint8_t*>(unterminated_fstr.data()),
                              unterminated_fstr.size());
        }
    }
}

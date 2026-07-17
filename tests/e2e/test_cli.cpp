#include "roxy/core/doctest/doctest.h"

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#ifdef _WIN32
#define popen _popen
#define pclose _pclose
#else
#include <sys/wait.h>
#endif

// ============================================================================
// CLI driver tests (src/roxy.cpp).
//
// The driver is its own executable and is not linked into roxy_tests, so the
// only way to exercise what it does with argv — building the `List<string>` for
// `main(args: List<string>)` — is to run the binary. ROXY_CLI_PATH is the built
// `roxy` target, supplied by CMake.
//
// These assert on *how the process ended*, not just its output: the bug this
// suite exists for produced completely correct stdout and then aborted during
// teardown, so a stdout-only check passed straight through it.
// ============================================================================

#ifdef ROXY_CLI_PATH

namespace {

struct CliRun {
    std::string stdout_output;
    int exit_code = -1;
    bool clean_exit = false;  // false when the process died on a signal
};

const char* cli_tmpdir() {
#ifdef _WIN32
    const char* t = getenv("TEMP");
    if (!t) t = getenv("TMP");
    if (!t) t = ".";
    return t;
#else
    const char* t = getenv("TMPDIR");
    if (!t) t = "/tmp";
    return t;
#endif
}

// Write `source` to a temp .roxy file, run the CLI on it with `extra_args`
// appended, and report how the process ended.
CliRun run_cli(const char* source, const char* extra_args) {
    CliRun result;

    char src_path[512];
    snprintf(src_path, sizeof(src_path), "%s/roxy_cli_test.roxy", cli_tmpdir());
    FILE* f = fopen(src_path, "w");
    if (!f) return result;
    fputs(source, f);
    fclose(f);

    // stdout is captured; stderr stays attached on purpose. Redirecting both
    // lets an intermediate shell fork and mask a signal death into a 128+signo
    // exit code — which is exactly the signal we need to see here. (Same
    // reasoning as the C-backend runner in test_helpers.cpp.)
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"%s%s",
             ROXY_CLI_PATH, src_path,
             extra_args && *extra_args ? " " : "",
             extra_args ? extra_args : "");

    FILE* pipe = popen(cmd, "r");
    if (!pipe) {
        remove(src_path);
        return result;
    }

    char buf[1024];
    while (fgets(buf, sizeof(buf), pipe)) {
        result.stdout_output.append(buf);
    }
    int status = pclose(pipe);
    remove(src_path);

#ifdef _WIN32
    // Abnormal termination (a failed assert/abort -> 0xC0000409, …) lands in the
    // NTSTATUS error-severity range and surfaces through _pclose as that code.
    if (((unsigned)status & 0xF0000000u) == 0xC0000000u) {
        result.clean_exit = false;
    } else {
        result.exit_code = status & 0xFF;
        result.clean_exit = true;
    }
#else
    if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
        result.clean_exit = true;
    } else {
        result.clean_exit = false;  // WIFSIGNALED: SIGABRT from the failed assert
    }
#endif
    return result;
}

}  // namespace

TEST_SUITE("E2E CLI") {

    TEST_CASE("main(args) exits cleanly") {
        // The args list is allocated by the driver before `vm_call` installs the
        // VM's context, while `args` is an owned `List` that main RAII-deletes
        // at scope exit — a free that runs under the VM's context and returns
        // the memory to the per-VM slab. Allocating outside that context falls
        // back to the malloc allocator, and the slab aborts on a pointer it
        // never produced ("SlabAllocator::free called with unknown pointer").
        // The abort lands *after* main's output, hence the clean_exit check.
        const char* source =
            "fun main(args: List<string>): i32 {\n"
            "    print(f\"argc={args.len()}\");\n"
            "    return 0;\n"
            "}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit);  // false => died on a signal (the abort)
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "argc=1\n");  // argv[0] is the source path
    }

    TEST_CASE("main(args) exits cleanly with an empty body") {
        // Nothing touches the list, so this isolates the alloc/free pairing from
        // anything the program does with it.
        const char* source = "fun main(args: List<string>) {\n}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit);
        CHECK(result.exit_code == 0);
    }

    TEST_CASE("main(args) receives the CLI arguments") {
        const char* source =
            "fun main(args: List<string>): i32 {\n"
            "    for (var i: i32 = 1; i < args.len(); i = i + 1) {\n"
            "        print(args[i]);\n"
            "    }\n"
            "    return 0;\n"
            "}\n";

        CliRun result = run_cli(source, "alpha beta");
        CHECK(result.clean_exit);
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "alpha\nbeta\n");
    }

    TEST_CASE("main() without args still exits cleanly") {
        // The no-args path never built a list and was never affected; this pins
        // that the context guard didn't disturb it.
        const char* source =
            "fun main(): i32 {\n"
            "    print(\"ok\");\n"
            "    return 0;\n"
            "}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit);
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "ok\n");
    }

    TEST_CASE("main()'s return value becomes the exit code") {
        const char* source = "fun main(): i32 {\n    return 3;\n}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit);
        CHECK(result.exit_code == 3);
    }

}  // TEST_SUITE("E2E CLI")

#endif  // ROXY_CLI_PATH

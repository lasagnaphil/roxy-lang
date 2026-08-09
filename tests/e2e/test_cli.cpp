#include "roxy/core/doctest/doctest.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
    bool clean_exit = false; // false when the process died on a signal
};

const char* cli_tmpdir() {
#ifdef _WIN32
    const char* t = getenv("TEMP");
    if (!t)
        t = getenv("TMP");
    if (!t)
        t = ".";
    return t;
#else
    const char* t = getenv("TMPDIR");
    if (!t)
        t = "/tmp";
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
    if (!f)
        return result;
    fputs(source, f);
    fclose(f);

    // stdout is captured; stderr stays attached on purpose. Redirecting both
    // lets an intermediate shell fork and mask a signal death into a 128+signo
    // exit code — which is exactly the signal we need to see here. (Same
    // reasoning as the C-backend runner in test_helpers.cpp.)
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "\"%s\" \"%s\"%s%s", ROXY_CLI_PATH, src_path,
             extra_args && *extra_args ? " " : "", extra_args ? extra_args : "");

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
        result.clean_exit = false; // WIFSIGNALED: SIGABRT from the failed assert
    }
#endif
    return result;
}

} // namespace

TEST_SUITE("E2E CLI") {

    TEST_CASE("main(args) exits cleanly") {
        // The args list is allocated by the driver before `vm_call` installs the
        // VM's context, while `args` is an owned `List` that main RAII-deletes
        // at scope exit — a free that runs under the VM's context and returns
        // the memory to the per-VM slab. Allocating outside that context falls
        // back to the malloc allocator, and the slab aborts on a pointer it
        // never produced ("SlabAllocator::free called with unknown pointer").
        // The abort lands *after* main's output, hence the clean_exit check.
        const char* source = "fun main(args: List<string>): i32 {\n"
                             "    print(f\"argc={args.len()}\");\n"
                             "    return 0;\n"
                             "}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit); // false => died on a signal (the abort)
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "argc=1\n"); // argv[0] is the source path
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
        const char* source = "fun main(args: List<string>): i32 {\n"
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
        const char* source = "fun main(): i32 {\n"
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

    // ── Whole-binary pipeline coverage ──────────────────────────────────────
    //
    // The parametric suites now run the optimizer too (test_helpers.cpp mirrors
    // Compiler::link_modules()), so these are no longer the only optimized-IR
    // coverage. They keep their own value: they exercise the *shipped binary*
    // driving Compiler::compile() end to end, which is what catches a harness
    // that has drifted from the real pipeline — exactly the failure that hid
    // the bug below.
    //
    // That bug: coroutine lowering minted result ValueIds with bare
    // `new_value()`, leaving `values_by_id` null for every instruction it
    // created. DCE's worklist dereferenced that null, so compiling *any*
    // coroutine segfaulted the compiler while the whole E2E suite stayed green.

    TEST_CASE("a coroutine compiles through the optimizing pipeline") {
        // Never called — this is about compiling the lowered IR at all, not
        // about running it.
        const char* source = "fun gen(): Coro<i32> { yield 1; }\n"
                             "fun main(): i32 {\n"
                             "    print(\"ok\");\n"
                             "    return 0;\n"
                             "}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit); // false => the compiler died (SIGSEGV in DCE)
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "ok\n");
    }

    TEST_CASE("a coroutine still produces correct values after optimization") {
        // Not just "doesn't crash": DCE/copy-prop/CSE now actually see the
        // coroutine's instructions, so this pins that they don't mangle the
        // resume state machine. countdown(3) yields 3, 2, 1.
        const char* source = "fun countdown(n: i32): Coro<i32> {\n"
                             "    var i: i32 = n;\n"
                             "    while (i > 0) { yield i; i = i - 1; }\n"
                             "}\n"
                             "fun main(): i32 {\n"
                             "    var c = countdown(3);\n"
                             "    var sum: i32 = 0;\n"
                             "    while (!c.done()) { sum = sum + c.resume(); }\n"
                             "    print(f\"sum={sum}\");\n"
                             "    return 0;\n"
                             "}\n";

        CliRun result = run_cli(source, "");
        CHECK(result.clean_exit);
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "sum=6\n");
    }

} // TEST_SUITE("E2E CLI")

#endif // ROXY_CLI_PATH

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Closures (commit 2 scope: zero-capture lambdas only)
// ============================================================================

TEST_CASE("E2E - Closure: lambda creation and immediate call") {
    SUBCASE("Single arg, expression body") {
        const char* source = R"(
            fun main() {
                var f = fun(x: i32): i32 => x + 1;
                print(f"{f(5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "6\n");
    }

    SUBCASE("Multi arg, expression body") {
        const char* source = R"(
            fun main() {
                var mul = fun(x: i32, y: i32): i32 => x * y;
                print(f"{mul(6, 7)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Block body with return") {
        const char* source = R"(
            fun main() {
                var g = fun(): i32 { return 42; };
                print(f"{g()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Void return") {
        const char* source = R"(
            fun main() {
                var greet = fun(name: string) { print(f"hello {name}"); };
                greet("world");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "hello world\n");
    }
}

TEST_CASE("E2E - Closure: higher-order functions") {
    SUBCASE("Pass closure as parameter") {
        const char* source = R"(
            fun apply(f: fun(i32) -> i32, x: i32): i32 {
                return f(x);
            }
            fun main() {
                print(f"{apply(fun(x: i32): i32 => x + 1, 5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "6\n");
    }

    SUBCASE("Closure called twice inside callee") {
        const char* source = R"(
            fun apply_twice(f: fun(i32) -> i32, x: i32): i32 {
                return f(f(x));
            }
            fun main() {
                print(f"{apply_twice(fun(x: i32): i32 => x * 2, 3)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "12\n");
    }

    SUBCASE("Return closure from function") {
        const char* source = R"(
            fun make_inc(): fun(i32) -> i32 {
                return fun(x: i32): i32 => x + 1;
            }
            fun main() {
                var inc = make_inc();
                print(f"{inc(99)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }
}

TEST_CASE("E2E - Closure: rejects unsupported features cleanly") {
    BumpAllocator allocator(65536);

    SUBCASE("Implicit capture of outer variable") {
        const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun(x: i32): i32 => x + n;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    SUBCASE("Explicit move capture (deferred to follow-up)") {
        const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun[move n](): i32 => n;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    SUBCASE("Function reference (deferred to follow-up)") {
        // Bare named-function-as-value is parsed and type-checks, but the IR
        // builder hasn't yet been taught to lower it (function references will
        // desugar to zero-capture lambdas in a follow-up commit).
        const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun main() {
                var f: fun(i32) -> i32 = double;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }
}

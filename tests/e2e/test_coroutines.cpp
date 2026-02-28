#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Coroutine Tests
// ============================================================================

TEST_CASE("E2E - Coroutine single yield") {
    const char* source = R"(
        fun single(): Coro<i32> {
            yield 42;
        }

        fun main(): i32 {
            var g = single();
            return g.resume();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Coroutine multiple yields") {
    const char* source = R"(
        fun triple(): Coro<i32> {
            yield 10;
            yield 20;
            yield 30;
        }

        fun main(): i32 {
            var g = triple();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a + b + c;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 60);
}

TEST_CASE("E2E - Coroutine done check") {
    const char* source = R"(
        fun one_val(): Coro<i32> {
            yield 99;
        }

        fun to_int(b: bool): i32 {
            if (b) { return 1; }
            return 0;
        }

        fun main(): i32 {
            var g = one_val();
            var before: i32 = to_int(g.done());
            var val: i32 = g.resume();
            var after_one: i32 = to_int(g.done());
            g.resume();
            var after_two: i32 = to_int(g.done());
            return before * 100 + after_one * 10 + after_two;
        }
    )";

    // before=0 (not done), after_one=0 (not done), after_two=1 (done)
    // Result: 0*100 + 0*10 + 1 = 1
    // before=0 (not done), after_one=0 (not done), after_two=1 (done)
    // Result: 0*100 + 0*10 + 1 = 1
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Coroutine with parameters") {
    const char* source = R"(
        fun add_offset(base: i32, offset: i32): Coro<i32> {
            yield base + offset;
            yield base + offset + 1;
        }

        fun main(): i32 {
            var g = add_offset(10, 5);
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 31);
}

TEST_CASE("E2E - Coroutine local variables across yields") {
    const char* source = R"(
        fun locals(): Coro<i32> {
            var x: i32 = 10;
            var y: i32 = 20;
            yield x + y;
            x = x + 1;
            yield x + y;
        }

        fun main(): i32 {
            var g = locals();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 61);
}

TEST_CASE("E2E - Coroutine yield in if/else") {
    const char* source = R"(
        fun conditional(flag: bool): Coro<i32> {
            if (flag) {
                yield 100;
            } else {
                yield 200;
            }
            yield 300;
        }

        fun main(): i32 {
            var g1 = conditional(true);
            var a: i32 = g1.resume();
            var b: i32 = g1.resume();

            var g2 = conditional(false);
            var c: i32 = g2.resume();
            var d: i32 = g2.resume();

            return a + b + c + d;
        }
    )";

    // g1 (flag=true): resume() → 100, resume() → 300. Total: 400
    // g2 (flag=false): resume() → 200, resume() → 300. Total: 500
    // Result: 400 + 500 = 900
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 900);
}

TEST_CASE("E2E - Coroutine error: yield outside coroutine") {
    const char* source = R"(
        fun not_a_coro(): i32 {
            yield 42;
            return 0;
        }

        fun main(): i32 {
            return not_a_coro();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Coroutine error: return with value") {
    const char* source = R"(
        fun bad_coro(): Coro<i32> {
            return 42;
        }

        fun main(): i32 {
            var g = bad_coro();
            return g.resume();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

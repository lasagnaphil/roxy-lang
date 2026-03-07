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

// ============================================================================
// Yield in loops
// ============================================================================

TEST_CASE("E2E - Coroutine yield in while loop") {
    const char* source = R"(
        fun counter(): Coro<i32> {
            var i: i32 = 0;
            while (i < 3) {
                yield i;
                i = i + 1;
            }
        }

        fun main(): i32 {
            var g = counter();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;
        }
    )";

    // a=0, b=1, c=2 → 0*100 + 1*10 + 2 = 12
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 12);
}

TEST_CASE("E2E - Coroutine yield in while loop with done check") {
    const char* source = R"(
        fun counter(): Coro<i32> {
            var i: i32 = 0;
            while (i < 2) {
                yield i;
                i = i + 1;
            }
        }

        fun to_int(b: bool): i32 {
            if (b) { return 1; }
            return 0;
        }

        fun main(): i32 {
            var g = counter();
            var a: i32 = g.resume();
            var d1: i32 = to_int(g.done());
            var b: i32 = g.resume();
            var d2: i32 = to_int(g.done());
            g.resume();
            var d3: i32 = to_int(g.done());
            return a * 1000 + b * 100 + d1 * 100 + d2 * 10 + d3;
        }
    )";

    // a=0, b=1, d1=0 (not done), d2=0 (not done), d3=1 (done after loop ends)
    // 0*1000 + 1*100 + 0*100 + 0*10 + 1 = 101
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 101);
}

TEST_CASE("E2E - Coroutine yield in while loop with break") {
    const char* source = R"(
        fun early_stop(): Coro<i32> {
            var i: i32 = 0;
            while (i < 10) {
                if (i == 2) {
                    break;
                }
                yield i;
                i = i + 1;
            }
            yield 99;
        }

        fun main(): i32 {
            var g = early_stop();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;
        }
    )";

    // Yields: 0, 1, then break, then 99
    // a=0, b=1, c=99 → 0*100 + 1*10 + 99 = 109
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 109);
}

TEST_CASE("E2E - Coroutine yield in while loop with continue") {
    const char* source = R"(
        fun skip_odds(): Coro<i32> {
            var i: i32 = 0;
            while (i < 6) {
                var cur: i32 = i;
                i = i + 1;
                if (cur == 1) {
                    continue;
                }
                if (cur == 3) {
                    continue;
                }
                if (cur == 5) {
                    continue;
                }
                yield cur;
            }
        }

        fun main(): i32 {
            var g = skip_odds();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;
        }
    )";

    // Yields: 0, 2, 4 (odd values skipped by continue)
    // a=0, b=2, c=4 → 0*100 + 2*10 + 4 = 24
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 24);
}

TEST_CASE("E2E - Coroutine yield in for loop") {
    const char* source = R"(
        fun range(n: i32): Coro<i32> {
            for (var i: i32 = 0; i < n; i = i + 1) {
                yield i;
            }
        }

        fun main(): i32 {
            var g = range(4);
            var sum: i32 = 0;
            sum = sum + g.resume();
            sum = sum + g.resume();
            sum = sum + g.resume();
            sum = sum + g.resume();
            return sum;
        }
    )";

    // Yields: 0, 1, 2, 3 → sum = 6
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 6);
}

TEST_CASE("E2E - Coroutine multiple yields in loop body") {
    const char* source = R"(
        fun double_yield(): Coro<i32> {
            var i: i32 = 0;
            while (i < 2) {
                yield i * 10;
                yield i * 10 + 1;
                i = i + 1;
            }
        }

        fun main(): i32 {
            var g = double_yield();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            var d: i32 = g.resume();
            return a * 1000 + b * 100 + c * 10 + d;
        }
    )";

    // i=0: yield 0, yield 1; i=1: yield 10, yield 11
    // a=0, b=1, c=10, d=11 → 0*1000 + 1*100 + 10*10 + 11 = 211
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 211);
}

TEST_CASE("E2E - Coroutine yield in nested loops") {
    const char* source = R"(
        fun matrix(): Coro<i32> {
            for (var i: i32 = 0; i < 2; i = i + 1) {
                for (var j: i32 = 0; j < 2; j = j + 1) {
                    yield i * 10 + j;
                }
            }
        }

        fun main(): i32 {
            var g = matrix();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            var d: i32 = g.resume();
            return a * 1000 + b * 100 + c * 10 + d;
        }
    )";

    // (0,0)=0, (0,1)=1, (1,0)=10, (1,1)=11
    // a=0, b=1, c=10, d=11 → 0*1000 + 1*100 + 10*10 + 11 = 211
    TestResult result = run_and_capture(source, "main", {}, true);
    CHECK(result.success);
    CHECK(result.value == 211);
}

// ============================================================================
// Yield in when statements
// ============================================================================

TEST_CASE("E2E - Coroutine yield in when statement") {
    const char* source = R"(
        enum Color { Red, Green, Blue }

        fun color_values(c: Color): Coro<i32> {
            when c {
                case Red:
                    yield 1;
                case Green:
                    yield 2;
                case Blue:
                    yield 3;
            }
            yield 0;
        }

        fun main(): i32 {
            var g1 = color_values(Color::Red);
            var a: i32 = g1.resume();
            var b: i32 = g1.resume();

            var g2 = color_values(Color::Blue);
            var c: i32 = g2.resume();
            var d: i32 = g2.resume();

            return a * 1000 + b * 100 + c * 10 + d;
        }
    )";

    // g1(Red): yield 1, yield 0 → a=1, b=0
    // g2(Blue): yield 3, yield 0 → c=3, d=0
    // 1*1000 + 0*100 + 3*10 + 0 = 1030
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1030);
}

// ============================================================================
// Deeply nested yield
// ============================================================================

TEST_CASE("E2E - Coroutine deeply nested yield") {
    const char* source = R"(
        fun deep(a: bool, b: bool): Coro<i32> {
            if (a) {
                if (b) {
                    yield 11;
                } else {
                    yield 10;
                }
            } else {
                if (b) {
                    yield 1;
                } else {
                    yield 0;
                }
            }
            yield 99;
        }

        fun main(): i32 {
            var g1 = deep(true, true);
            var v1: i32 = g1.resume();
            var e1: i32 = g1.resume();

            var g2 = deep(false, true);
            var v2: i32 = g2.resume();
            var e2: i32 = g2.resume();

            return v1 * 1000 + e1 * 100 + v2 * 10 + e2;
        }
    )";

    // g1(true,true): yield 11, yield 99 → v1=11, e1=99
    // g2(false,true): yield 1, yield 99 → v2=1, e2=99
    // 11*1000 + 99*100 + 1*10 + 99 = 11000 + 9900 + 10 + 99 = 21009
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 21009);
}

// ============================================================================
// Yield in try/catch
// ============================================================================

TEST_CASE("E2E - Coroutine yield in try block") {
    const char* source = R"(
        fun gen(): Coro<i32> {
            try {
                yield 42;
            } catch (e) {
            }
        }

        fun main(): i32 {
            var g = gen();
            return g.resume();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Coroutine yield in catch block") {
    const char* source = R"(
        struct MyErr {}
        fun MyErr.message(): string for Exception {
            return "err";
        }

        fun gen(): Coro<i32> {
            try {
                throw MyErr {};
            } catch (e: MyErr) {
                yield 42;
            }
        }

        fun main(): i32 {
            var g = gen();
            return g.resume();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Coroutine error: yield in finally block") {
    const char* source = R"(
        fun bad_coro(): Coro<i32> {
            try {
                var x: i32 = 1;
            } catch (e) {
            } finally {
                yield 42;
            }
        }

        fun main(): i32 {
            var g = bad_coro();
            return g.resume();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Coroutine yield in try, no exception") {
    const char* source = R"(
        fun gen(): Coro<i32> {
            var result: i32 = 0;
            try {
                result = 10;
                yield result;
                result = 20;
                yield result;
            } catch (e) {
                result = -1;
            }
            yield result;
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;
        }
    )";

    // a=10, b=20, c=20 (no exception, so result stays 20 after try)
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1220);
}

TEST_CASE("E2E - Coroutine multiple yields in try") {
    const char* source = R"(
        fun gen(): Coro<i32> {
            try {
                yield 1;
                yield 2;
                yield 3;
            } catch (e) {
            }
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 123);
}

TEST_CASE("E2E - Coroutine yield in catch after throw") {
    const char* source = R"(
        struct MyErr {
            val: i32;
        }
        fun MyErr.message(): string for Exception {
            return "err";
        }

        fun gen(): Coro<i32> {
            try {
                throw MyErr { val = 99 };
            } catch (e: MyErr) {
                yield e.val;
                yield e.val + 1;
            }
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a * 100 + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 9900 + 100);
}

TEST_CASE("E2E - Coroutine yield in try with loop") {
    const char* source = R"(
        fun gen(): Coro<i32> {
            try {
                for (var i: i32 = 0; i < 3; i = i + 1) {
                    yield i * 10;
                }
            } catch (e) {
            }
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a + b + c;
        }
    )";

    // 0 + 10 + 20 = 30
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 30);
}

// ============================================================================
// Coroutine Memory Management Tests
// ============================================================================

TEST_CASE("E2E - Coroutine primitive cleanup") {
    // Verify primitive-only Coro<i32> compiles and runs correctly.
    // The heap-allocated state struct is freed at scope exit.
    const char* source = R"(
        fun counter(): Coro<i32> {
            yield 10;
            yield 20;
        }

        fun main(): i32 {
            var g = counter();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 30);
}

TEST_CASE("E2E - Coroutine uniq promoted, run to completion") {
    // A uniq variable captured across a yield point becomes a promoted field.
    // When the coroutine runs to completion, inline cleanup frees the uniq,
    // and the destructor (called at Coro scope exit) sees null and skips it.
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource() {
            print(f"{"dtor"}");
        }

        fun gen(): Coro<i32> {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            yield r.value;
            yield r.value + 1;
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            // Resume once more to reach done state (triggers inline cleanup of r)
            g.resume();
            return a * 100 + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 4243);
    // Destructor should be called exactly once (inline cleanup on done path)
    CHECK(result.stdout_output == "dtor\n");
}

TEST_CASE("E2E - Coroutine uniq promoted, early drop") {
    // Drop the Coro before it reaches done. The destructor should clean up
    // the promoted uniq field that hasn't been freed by inline cleanup.
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource() {
            print(f"{"freed"}");
        }

        fun gen(): Coro<i32> {
            var r: uniq Resource = uniq Resource();
            r.value = 99;
            yield r.value;
            yield r.value + 1;
        }

        fun main(): i32 {
            var result: i32 = 0;
            {
                var g = gen();
                result = g.resume();
                // g goes out of scope here without reaching done
                // The destructor should clean up the promoted uniq field
            }
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
    // Destructor should be called by the coroutine's destructor
    CHECK(result.stdout_output == "freed\n");
}

TEST_CASE("E2E - Coroutine uniq parameter") {
    // A uniq parameter to a coroutine is captured in the state struct.
    // Cleanup should free it when the coroutine is destroyed.
    const char* source = R"(
        struct Data {
            value: i32;
        }

        fun delete Data() {
            print(f"{"~Data"}");
        }

        fun gen(d: uniq Data): Coro<i32> {
            yield d.value;
            yield d.value * 2;
        }

        fun main(): i32 {
            var d: uniq Data = uniq Data();
            d.value = 5;
            var g = gen(d);
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            g.resume();  // Run to completion
            return a * 10 + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 60);
    CHECK(result.stdout_output == "~Data\n");
}

TEST_CASE("E2E - Coroutine mixed primitive and uniq promoted") {
    // Only noncopyable fields get cleanup. Primitive promoted variables
    // should work alongside uniq promoted variables.
    const char* source = R"(
        struct Counter {
            count: i32;
        }

        fun delete Counter() {
            print(f"{"~Counter"}");
        }

        fun gen(): Coro<i32> {
            var c: uniq Counter = uniq Counter();
            c.count = 0;
            var multiplier: i32 = 10;
            c.count = c.count + 1;
            yield c.count * multiplier;
            c.count = c.count + 1;
            yield c.count * multiplier;
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            g.resume();  // Run to completion
            return a + b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 30);
    CHECK(result.stdout_output == "~Counter\n");
}

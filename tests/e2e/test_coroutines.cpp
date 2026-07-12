#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

#include <string>

using namespace rx;

// ============================================================================
// Coroutine Tests
// ============================================================================

TEST_SUITE("E2E Coroutines") {

    TEST_CASE_TEMPLATE("Coroutine single yield", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun single(): Coro<i32> {
            yield 42;
        }

        fun main(): i32 {
            var g = single();
            return g.resume();
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Coroutine multiple yields", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 60);
    }

    TEST_CASE_TEMPLATE("Coroutine done check", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Coroutine with parameters", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 31);
    }

    TEST_CASE_TEMPLATE("Coroutine local variables across yields", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 61);
    }

    TEST_CASE("Coroutine yield in if/else") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 900);
    }

    TEST_CASE_TEMPLATE("Coroutine error: yield outside coroutine", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun not_a_coro(): i32 {
            yield 42;
            return 0;
        }

        fun main(): i32 {
            return not_a_coro();
        }
    )";

        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE_TEMPLATE("Coroutine error: return with value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bad_coro(): Coro<i32> {
            return 42;
        }

        fun main(): i32 {
            var g = bad_coro();
            return g.resume();
        }
    )";

        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    // ============================================================================
    // Yield in loops
    // ============================================================================

    TEST_CASE_TEMPLATE("Coroutine yield in while loop", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 12);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in while loop with done check", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 101);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in while loop with break", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 109);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in while loop with continue", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 24);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in for loop", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE_TEMPLATE("Coroutine multiple yields in loop body", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 211);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in nested loops", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 211);
    }

    // ============================================================================
    // Yield in when statements
    // ============================================================================

    TEST_CASE("Coroutine yield in when statement") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1030);
    }

    // ============================================================================
    // Deeply nested yield
    // ============================================================================

    TEST_CASE("Coroutine deeply nested yield") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 21009);
    }

    // ============================================================================
    // Yield in try/catch
    // ============================================================================

    TEST_CASE_TEMPLATE("Coroutine yield in try block", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in catch block", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Coroutine error: yield in finally block", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE("Coroutine yield in try, no exception") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1220);
    }

    TEST_CASE_TEMPLATE("Coroutine multiple yields in try", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 123);
    }

    TEST_CASE("Coroutine yield in catch after throw") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 9900 + 100);
    }

    TEST_CASE_TEMPLATE("Coroutine yield in try with loop", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 30);
    }

    // ============================================================================
    // Coroutine Memory Management Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Coroutine primitive cleanup", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 30);
    }

    TEST_CASE("Coroutine uniq promoted, run to completion") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4243);
        // Destructor should be called exactly once (inline cleanup on done path)
        CHECK(result.stdout_output == "dtor\n");
    }

    TEST_CASE_TEMPLATE("Coroutine uniq promoted, early drop", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
        // Destructor should be called by the coroutine's destructor
        CHECK(result.stdout_output == "freed\n");
    }

    TEST_CASE_TEMPLATE("Coroutine uniq parameter", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 60);
        CHECK(result.stdout_output == "~Data\n");
    }

    TEST_CASE_TEMPLATE("Coroutine mixed primitive and uniq promoted", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 30);
        CHECK(result.stdout_output == "~Counter\n");
    }

    // ============================================================================
    // List/Map Cleanup in Coroutine Destructors
    // ============================================================================

    TEST_CASE("Coroutine List<uniq T> cleanup on completion") {  // VM-only: C backend: coroutine uniq-field (List/Map) cleanup gap
        const char* source = R"CODE(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"~Resource({self.id})");
        }

        fun gen(): Coro<i32> {
            var items: List<uniq Resource> = List<uniq Resource>();
            var r1: uniq Resource = uniq Resource();
            r1.id = 1;
            items.push(r1);
            var r2: uniq Resource = uniq Resource();
            r2.id = 2;
            items.push(r2);
            yield items.len();
            var r3: uniq Resource = uniq Resource();
            r3.id = 3;
            items.push(r3);
            yield items.len();
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            g.resume();
            return a * 10 + b;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 23);
        // All three resources should be cleaned up (order: 1, 2, 3 from list iteration)
        CHECK(result.stdout_output == "~Resource(1)\n~Resource(2)\n~Resource(3)\n");
    }

    TEST_CASE("Coroutine List<uniq T> cleanup on early drop") {  // VM-only: C backend: coroutine uniq-field (List/Map) cleanup gap
        const char* source = R"CODE(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"~Resource({self.id})");
        }

        fun gen(): Coro<i32> {
            var items: List<uniq Resource> = List<uniq Resource>();
            var r1: uniq Resource = uniq Resource();
            r1.id = 10;
            items.push(r1);
            var r2: uniq Resource = uniq Resource();
            r2.id = 20;
            items.push(r2);
            yield items.len();
            yield items.len();
        }

        fun main(): i32 {
            var result: i32 = 0;
            {
                var g = gen();
                result = g.resume();
                // g goes out of scope before done — destructor must clean up list elements
            }
            return result;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
        // Both resources should be cleaned up by the coroutine destructor
        CHECK(result.stdout_output == "~Resource(10)\n~Resource(20)\n");
    }

    TEST_CASE("Coroutine Map<string, uniq T> cleanup on completion") {  // VM-only: C backend: coroutine uniq-field (List/Map) cleanup gap
        const char* source = R"CODE(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"~Resource({self.id})");
        }

        fun gen(): Coro<i32> {
            var m: Map<string, uniq Resource> = Map<string, uniq Resource>();
            var r1: uniq Resource = uniq Resource();
            r1.id = 100;
            m.insert("a", r1);
            var r2: uniq Resource = uniq Resource();
            r2.id = 200;
            m.insert("b", r2);
            yield m.len();
            var r3: uniq Resource = uniq Resource();
            r3.id = 300;
            m.insert("c", r3);
            yield m.len();
        }

        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            g.resume();
            return a * 10 + b;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 23);
        // All three resources should be cleaned up (order depends on hash table bucket layout)
        CHECK(result.stdout_output.find("~Resource(100)") != std::string::npos);
        CHECK(result.stdout_output.find("~Resource(200)") != std::string::npos);
        CHECK(result.stdout_output.find("~Resource(300)") != std::string::npos);
    }

    TEST_CASE("Coroutine Map<string, uniq T> cleanup on early drop") {  // VM-only: C backend: coroutine uniq-field (List/Map) cleanup gap
        const char* source = R"CODE(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"~Resource({self.id})");
        }

        fun gen(): Coro<i32> {
            var m: Map<string, uniq Resource> = Map<string, uniq Resource>();
            var r1: uniq Resource = uniq Resource();
            r1.id = 10;
            m.insert("x", r1);
            var r2: uniq Resource = uniq Resource();
            r2.id = 20;
            m.insert("y", r2);
            yield m.len();
            yield m.len();
        }

        fun main(): i32 {
            var result: i32 = 0;
            {
                var g = gen();
                result = g.resume();
                // g goes out of scope before done — destructor must clean up map values
            }
            return result;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
        // Both resources should be cleaned up by the coroutine destructor
        CHECK(result.stdout_output.find("~Resource(10)") != std::string::npos);
        CHECK(result.stdout_output.find("~Resource(20)") != std::string::npos);
    }

    // ── `ref` parameter counting (lifetimes.md §13) ──
    // A `ref` param promoted into the coroutine's heap state struct is a counted
    // borrow held for the coro's lifetime: ref_inc when stored into the state at
    // creation, ref_dec in the generated `$$delete`. So holding a borrow in a live
    // coroutine keeps the owner alive (deleting it early traps), and the count is
    // balanced whether the coro completes or is destroyed mid-iteration.

    TEST_CASE_TEMPLATE("Coroutine ref param: balanced across resume + teardown", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun gen(r: ref P): Coro<i32> {
            yield r.x;
            yield r.x + 1;
        }
        fun main(): i32 {
            var o: uniq P = uniq P();
            o.x = 5;
            var c = gen(o);
            var a: i32 = c.resume();   // 5
            var b: i32 = c.resume();   // 6
            return a + b;
            // teardown (LIFO): coro destroyed first → ref_dec releases the borrow;
            // then o is deletable (count 0). Balanced even though the coro never
            // ran to completion.
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 11);
    }

    TEST_CASE("Coroutine ref param: deleting the owner while the coro is live traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        // The borrow is acquired at creation, so the owner can't be freed while the
        // coroutine could still observe it — even before the first resume.
        const char* before_resume = R"(
        struct P { x: i32; }
        fun gen(r: ref P): Coro<i32> { yield r.x; yield r.x; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            var c = gen(o);   // borrow counted at creation
            delete o;         // still borrowed by the coro → traps
            return 0;
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, before_resume) != nullptr);
        CHECK(VMBackend::run(before_resume).success == false);

        // Same, after resuming once (the coro is suspended, still holding the ref).
        const char* mid_iteration = R"(
        struct P { x: i32; }
        fun gen(r: ref P): Coro<i32> { yield r.x; yield r.x; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            o.x = 5;
            var c = gen(o);
            var a: i32 = c.resume();
            delete o;         // coro suspended mid-iteration, still borrows o → traps
            return a;
        }
    )";
        CHECK(VMBackend::run(mid_iteration).success == false);
    }

    // ========================================================================
    // First-class coroutine values (pass / return / store an erased Coro<T>).
    // These exercise dynamic resume/done dispatch: the concrete coroutine
    // function is not known at the call site.
    // ========================================================================

    TEST_CASE_TEMPLATE("Coroutine returned from a wrapper function", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun counter(): Coro<i32> {
            yield 1;
            yield 2;
        }
        fun make(): Coro<i32> {
            return counter();
        }
        fun main(): i32 {
            var g = make();
            return g.resume() + g.resume();   // 1 + 2
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Coroutine passed to a function (moved, erased param)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun gen(): Coro<i32> {
            yield 7;
            yield 8;
        }
        fun first(c: Coro<i32>): i32 {
            return c.resume();   // c owned here; deleted (while suspended) at return
        }
        fun main(): i32 {
            var g = gen();
            return first(g);     // moves g into first
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("Coroutine dynamic dispatch across source functions", Backend, RX_E2E_BACKENDS) {
        // sum_two receives Coro<i32> values produced by two DIFFERENT coroutine
        // functions — the resume target cannot be statically bound.
        const char* source = R"(
        fun ones(): Coro<i32> {
            yield 1;
            yield 1;
        }
        fun twos(): Coro<i32> {
            yield 2;
            yield 2;
        }
        fun sum_two(c: Coro<i32>): i32 {
            return c.resume() + c.resume();
        }
        fun main(): i32 {
            return sum_two(ones()) + sum_two(twos());   // (1+1) + (2+2)
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE_TEMPLATE("Coroutine yielding a struct, passed erased", Backend, RX_E2E_BACKENDS) {
        // Resume returns a small struct through CALL_INDIRECT (the erased-value
        // dispatch path), exercising small-struct return unpacking.
        const char* source = R"(
        struct P { x: i32 = 0; y: i32 = 0; }
        fun points(): Coro<P> {
            yield P { x = 1, y = 2 };
            yield P { x = 3, y = 4 };
        }
        fun sum_first(c: Coro<P>): i32 {
            var p: P = c.resume();
            return p.x + p.y;
        }
        fun main(): i32 {
            return sum_first(points());   // 1 + 2
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Coroutine stored in annotated var, resume + done", Backend, RX_E2E_BACKENDS) {
        // A `Coro<i32>` annotation resolves to the interned generic type (empty
        // func_name) — resume/done must still work via dynamic dispatch.
        const char* source = R"(
        fun gen(): Coro<i32> {
            yield 5;
        }
        fun main(): i32 {
            var g: Coro<i32> = gen();
            var v: i32 = g.resume();
            g.resume();                 // run past the last yield
            var d: i32 = 0;
            if (g.done()) { d = 1; }
            return v * 10 + d;          // 51
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 51);
    }

    TEST_CASE_TEMPLATE("Erased coroutine dropped while suspended runs destructor", Backend, RX_E2E_BACKENDS) {
        // take_first resumes once then drops the coroutine mid-iteration. The
        // erased Coro<i32> must run its state-struct destructor (dispatched by
        // runtime identity, since the concrete type is unknown at the drop site)
        // so the promoted `uniq R` is cleaned up exactly once — observed via the
        // destructor's print.
        const char* source = R"(
        struct R { n: i32 = 0; }
        fun delete R() { print(f"drop R"); }

        fun gen(): Coro<i32> {
            var r: uniq R = uniq R();   // promoted owned local; cleaned by state dtor
            r.n = 1;
            yield r.n;
            yield 2;
            yield 3;
        }
        fun take_first(c: Coro<i32>): i32 {
            return c.resume();
        }
        fun main(): i32 {
            return take_first(gen());
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
        CHECK(result.stdout_output == "drop R\n");
    }

    // ========================================================================
    // Coroutine methods (`fun S.count(): Coro<T>`). `self` is captured into the
    // coroutine state struct like any `ref` parameter.
    // ========================================================================

    TEST_CASE_TEMPLATE("Coroutine method basic self yield", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct S { n: i32 = 0; }
        fun S.count(): Coro<i32> {
            yield self.n;
        }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 3;
            var c = s.count();
            return c.resume();
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Coroutine method state across yields", Backend, RX_E2E_BACKENDS) {
        // self + a promoted local both survive across yields. (Asserts on stdout
        // so the multi-value sequence isn't clamped by the C backend's exit code.)
        const char* source = R"(
        struct S { n: i32 = 0; }
        fun S.countdown(): Coro<i32> {
            var i: i32 = self.n;
            while (i > 0) {
                yield i;
                i = i - 1;
            }
        }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 3;
            var c = s.countdown();
            print(f"{c.resume()}");   // 3
            print(f"{c.resume()}");   // 2
            print(f"{c.resume()}");   // 1
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n2\n1\n");
    }

    TEST_CASE_TEMPLATE("Coroutine method done to completion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct S { n: i32 = 0; }
        fun S.once(): Coro<i32> { yield self.n; }
        fun to_int(b: bool): i32 { if (b) { return 1; } return 0; }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 7;
            var c = s.once();
            print(f"{to_int(c.done())}");   // 0 (not done)
            print(f"{c.resume()}");         // 7
            c.resume();                     // run past last yield
            print(f"{to_int(c.done())}");   // 1 (done)
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n7\n1\n");
    }

    TEST_CASE_TEMPLATE("Coroutine method two live instances", Backend, RX_E2E_BACKENDS) {
        // Two coroutine values from the same method on different receivers keep
        // independent state.
        const char* source = R"(
        struct Counter { start: i32 = 0; }
        fun Counter.gen(): Coro<i32> {
            var i: i32 = self.start;
            while (i < self.start + 100) {
                yield i;
                i = i + 1;
            }
        }
        fun main(): i32 {
            var a: uniq Counter = uniq Counter();
            a.start = 10;
            var b: uniq Counter = uniq Counter();
            b.start = 20;
            var ca = a.gen();
            var cb = b.gen();
            var r: i32 = 0;
            r = r + ca.resume();   // 10
            r = r + cb.resume();   // 20
            r = r + ca.resume();   // 11
            r = r + cb.resume();   // 21
            return r;              // 62
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 62);
    }

    TEST_CASE_TEMPLATE("Coroutine method inherited field and self method call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Base { base_val: i32 = 0; }
        fun Base.get_base(): i32 { return self.base_val; }
        struct Derived : Base { extra: i32 = 0; }
        fun Derived.gen(): Coro<i32> {
            yield self.get_base();   // call an inherited method from the coroutine
            yield self.extra;        // read own field
            yield self.base_val;     // read inherited field directly
        }
        fun main(): i32 {
            var d: uniq Derived = uniq Derived();
            d.base_val = 5;
            d.extra = 7;
            var c = d.gen();
            print(f"{c.resume()}");   // 5 (inherited method)
            print(f"{c.resume()}");   // 7 (own field)
            print(f"{c.resume()}");   // 5 (inherited field)
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "5\n7\n5\n");
    }

    TEST_CASE_TEMPLATE("Coroutine-returning method that forwards (no yield)", Backend, RX_E2E_BACKENDS) {
        // A non-yielding Coro<T>-returning method is an ordinary method that
        // returns a first-class coroutine value. Also confirms build_method uses
        // the MethodInfo return type (resolve_return_type would give `void`).
        const char* source = R"(
        fun free_gen(): Coro<i32> { yield 1; yield 2; }
        struct Factory { unused: i32 = 0; }
        fun Factory.make(): Coro<i32> {
            return free_gen();
        }
        fun main(): i32 {
            var f: uniq Factory = uniq Factory();
            var c = f.make();
            return c.resume() + c.resume();   // 3
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Coroutine method balanced teardown", Backend, RX_E2E_BACKENDS) {
        // The coroutine value and its receiver both live to scope end; LIFO
        // teardown deletes the coroutine (releasing its self-borrow) before the
        // receiver — no trap.
        const char* source = R"(
        struct S { n: i32 = 0; }
        fun S.gen(): Coro<i32> { yield self.n; yield self.n; }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 4;
            var c = s.gen();
            var v: i32 = c.resume();
            return v;   // 4
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4);
    }

    TEST_CASE("Coroutine method self-borrow traps if receiver deleted while alive") {
        // The coroutine borrows `self` for its whole lifetime; deleting the
        // receiver while the coroutine is still alive must trap. (VM-only: asserts
        // a runtime trap.)
        const char* before_resume = R"(
        struct S { n: i32 = 0; }
        fun S.gen(): Coro<i32> { yield self.n; yield self.n; }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 4;
            var c = s.gen();
            delete s;          // c still borrows s → trap
            return 0;
        }
    )";
        CHECK(VMBackend::run(before_resume).success == false);

        const char* mid_iteration = R"(
        struct S { n: i32 = 0; }
        fun S.gen(): Coro<i32> { yield self.n; yield self.n; }
        fun main(): i32 {
            var s: uniq S = uniq S();
            s.n = 4;
            var c = s.gen();
            var a: i32 = c.resume();   // suspended mid-iteration, still borrows s
            delete s;                  // trap
            return a;
        }
    )";
        CHECK(VMBackend::run(mid_iteration).success == false);
    }

}  // TEST_SUITE("E2E Coroutines")

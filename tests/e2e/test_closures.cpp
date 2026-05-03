#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Closures
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

TEST_CASE("E2E - Closure: implicit copy capture") {
    SUBCASE("Capture i32 by value") {
        const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun(): i32 => n + 1;
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "11\n");
    }

    SUBCASE("Capture is by value, not by reference") {
        // Mutating the outer variable after capture must NOT affect the closure.
        const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun(): i32 => n;
                n = 99;
                print(f"{f()}");
                print(f"{n}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n99\n");
    }

    SUBCASE("Capture f64") {
        const char* source = R"(
            fun main() {
                var pi: f64 = 3.14;
                var f = fun(): f64 => pi;
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "3.14\n");
    }

    SUBCASE("Capture multiple variables") {
        const char* source = R"(
            fun main() {
                var x: i32 = 100;
                var y: i32 = 50;
                var add = fun(a: i32): i32 => a + x + y;
                print(f"{add(7)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "157\n");
    }

    SUBCASE("Capture used multiple times in body") {
        // Each reference to `n` rewrites to `__env.n`; only one capture entry
        // should be added (dedup'd by Symbol*).
        const char* source = R"(
            fun main() {
                var n: i32 = 5;
                var f = fun(): i32 => n + n + n;
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "15\n");
    }

    SUBCASE("Closure returned from function captures parameter") {
        const char* source = R"(
            fun make_adder(n: i32): fun(i32) -> i32 {
                return fun(x: i32): i32 => x + n;
            }
            fun main() {
                var add5 = make_adder(5);
                print(f"{add5(3)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "8\n");
    }
}

TEST_CASE("E2E - Closure: explicit move capture") {
    SUBCASE("Move uniq T into closure") {
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 42 };
                var f = fun[move c](): i32 => c.value;
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Move consumes outer (use-after-move)") {
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 42 };
                var f = fun[move c](): i32 => c.value;
                print(f"{c.value}");
            }
        )";
        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail: 'c' is moved
    }
}

TEST_CASE("E2E - Closure: capture rule errors") {
    BumpAllocator allocator(65536);

    SUBCASE("Implicit capture of noncopyable errors with hint") {
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 1 };
                var f = fun(): i32 => c.value;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Must error: needs [move c]
    }

    SUBCASE("[move] on copyable type errors") {
        const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun[move n](): i32 => n;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // [move] reserved for noncopyables
    }

    SUBCASE("[move] of unknown variable errors") {
        const char* source = R"(
            fun main() {
                var f = fun[move ghost](): i32 => 0;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }
}

TEST_CASE("E2E - Closure: function references") {
    SUBCASE("Bare function name to typed variable") {
        const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun main() {
                var f: fun(i32) -> i32 = double;
                print(f"{f(21)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Type inference from function reference") {
        const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun main() {
                var f = double;
                print(f"{f(7)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "14\n");
    }

    SUBCASE("Pass function reference to higher-order") {
        const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun triple(x: i32): i32 { return x * 3; }
            fun apply(f: fun(i32) -> i32, x: i32): i32 {
                return f(x);
            }
            fun main() {
                print(f"{apply(double, 5)}");
                print(f"{apply(triple, 5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n15\n");
    }

    SUBCASE("Multiple references to same function reuse one trampoline") {
        // Cache dedup behavior — both bindings should compile to the same
        // synthesized trampoline and produce correct results.
        const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun main() {
                var a = double;
                var b = double;
                print(f"{a(3)}");
                print(f"{b(4)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "6\n8\n");
    }

    SUBCASE("Void-returning function reference") {
        const char* source = R"(
            fun greet(name: string) { print(f"hi {name}"); }
            fun main() {
                var g = greet;
                g("world");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "hi world\n");
    }
}

TEST_CASE("E2E - Closure: nested closures") {
    SUBCASE("Inner captures outer's parameter (make_adder)") {
        const char* source = R"(
            fun make_adder(x: i32): fun(i32) -> i32 {
                return fun(y: i32): i32 => x + y;
            }
            fun main() {
                var add5 = make_adder(5);
                print(f"{add5(3)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "8\n");
    }

    SUBCASE("Curried two-level closure") {
        const char* source = R"(
            fun make_curried(): fun(i32) -> fun(i32) -> i32 {
                return fun(x: i32): fun(i32) -> i32 {
                    return fun(y: i32): i32 => x + y;
                };
            }
            fun main() {
                var c = make_curried();
                print(f"{c(10)(20)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    SUBCASE("Three-level nesting with transitive capture") {
        // n is captured by all three lambdas; the innermost reads it through
        // a chain of __env field accesses (innermost env <- middle env <- outer env <- main local).
        const char* source = R"(
            fun main() {
                var n: i32 = 100;
                var f = fun(): fun(i32) -> fun(i32) -> i32 {
                    return fun(a: i32): fun(i32) -> i32 {
                        return fun(b: i32): i32 => n + a + b;
                    };
                };
                print(f"{f()(1)(2)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "103\n");
    }

    SUBCASE("Inner captures local from outer's body, not parameter") {
        const char* source = R"(
            fun main() {
                var seed: i32 = 7;
                var make = fun(): fun(i32) -> i32 {
                    return fun(x: i32): i32 => seed * x;
                };
                var triple_seed = make();
                print(f"{triple_seed(3)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }
}

TEST_CASE("E2E - Closure: self capture") {
    SUBCASE("Implicit ref-self on noncopyable struct") {
        // Noncopyable struct ⇒ heap-only ⇒ ref counting protects; no runtime check.
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun delete Counter() {}
            fun Counter.make_getter(): fun() -> i32 {
                return fun(): i32 => self.value;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 42 };
                var g = c.make_getter();
                print(f"{g()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Implicit ref-self on copyable + uniq receiver passes heap check") {
        const char* source = R"(
            struct V {
                x: i32 = 0;
            }
            fun V.make(): fun() -> i32 {
                return fun(): i32 => self.x;
            }
            fun main() {
                var u: uniq V = uniq V { x = 99 };
                var f = u.make();
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "99\n");
    }

    SUBCASE("Implicit ref-self on copyable + stack receiver triggers runtime trap") {
        const char* source = R"(
            struct V {
                x: i32 = 0;
            }
            fun V.make(): fun() -> i32 {
                return fun(): i32 => self.x;
            }
            fun main() {
                var v: V = V { x = 7 };
                var f = v.make();
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK_FALSE(result.success);  // Runtime trap from ASSERT_HEAP
    }

    SUBCASE("[copy self] on copyable: snapshot semantics") {
        // The closure holds a value snapshot; mutating the original after
        // construction must not affect what the closure observes.
        const char* source = R"(
            struct Vec2 {
                x: i32 = 0;
                y: i32 = 0;
            }
            fun Vec2.snapshot(): fun() -> i32 {
                return fun[copy self](): i32 => self.x + self.y;
            }
            fun main() {
                var v: Vec2 = Vec2 { x = 3, y = 4 };
                var f = v.snapshot();
                v.x = 999;
                v.y = 999;
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "7\n");
    }

    SUBCASE("[weak self] on noncopyable struct") {
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun delete Counter() {}
            fun Counter.make_getter(): fun() -> i32 {
                return fun[weak self](): i32 => self.value;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 99 };
                var f = c.make_getter();
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "99\n");
    }

    SUBCASE("[weak self] on copyable + uniq receiver passes heap check") {
        const char* source = R"(
            struct V {
                x: i32 = 0;
            }
            fun V.make(): fun() -> i32 {
                return fun[weak self](): i32 => self.x;
            }
            fun main() {
                var u: uniq V = uniq V { x = 21 };
                var f = u.make();
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("[weak self] on copyable + stack receiver triggers runtime trap") {
        const char* source = R"(
            struct V {
                x: i32 = 0;
            }
            fun V.make(): fun() -> i32 {
                return fun[weak self](): i32 => self.x;
            }
            fun main() {
                var v: V = V { x = 5 };
                var f = v.make();
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("E2E - Closure: self capture compile-time rejections") {
    BumpAllocator allocator(65536);

    SUBCASE("[copy self] on noncopyable struct") {
        const char* source = R"(
            struct N {
                v: i32 = 0;
            }
            fun delete N() {}
            fun N.bad(): fun() -> i32 {
                return fun[copy self](): i32 => self.v;
            }
            fun main() {}
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    SUBCASE("[copy self] outside of a method") {
        const char* source = R"(
            fun main() {
                var f = fun[copy self](): i32 => 0;
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

}

TEST_CASE("E2E - Closure: nested self capture") {
    SUBCASE("Nested [copy self] on copyable + uniq receiver") {
        // Outer takes implicit ref-self (heap check passes for uniq); inner's
        // [copy self] reads via outer's __env.__self and snapshots into its
        // own env field (value Vec2).
        const char* source = R"(
            struct Vec2 {
                x: i32 = 0;
                y: i32 = 0;
            }
            fun Vec2.factory(): fun() -> fun() -> i32 {
                return fun(): fun() -> i32 {
                    return fun[copy self](): i32 => self.x + self.y;
                };
            }
            fun main() {
                var v: uniq Vec2 = uniq Vec2 { x = 3, y = 4 };
                var f = v.factory();
                print(f"{f()()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "7\n");
    }

    SUBCASE("Nested [weak self] on noncopyable") {
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun delete Counter() {}
            fun Counter.factory(): fun() -> fun() -> i32 {
                return fun(): fun() -> i32 {
                    return fun[weak self](): i32 => self.value;
                };
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 99 };
                var f = c.factory();
                print(f"{f()()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "99\n");
    }

    SUBCASE("Nested implicit ref-self propagates through outer chain") {
        // Innermost references `self`; analyze_this_expr propagates ref-self
        // implicitly through every enclosing lambda's env so the chain works.
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun delete Counter() {}
            fun Counter.factory(): fun() -> fun() -> i32 {
                return fun(): fun() -> i32 {
                    return fun(): i32 => self.value;
                };
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 21 };
                var f = c.factory();
                print(f"{f()()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("Nested [copy self] on copyable + stack receiver still traps") {
        // Outer's implicit ref-self capture has needs_heap_check = true; on
        // a stack receiver the runtime trap fires, even though the inner
        // wants snapshot semantics. Use uniq receiver to opt into the chain.
        const char* source = R"(
            struct V {
                x: i32 = 0;
            }
            fun V.factory(): fun() -> fun() -> i32 {
                return fun(): fun() -> i32 {
                    return fun[copy self](): i32 => self.x;
                };
            }
            fun main() {
                var v: V = V { x = 7 };
                var f = v.factory();
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK_FALSE(result.success);
    }
}

TEST_CASE("E2E - Closure: rejects unsupported features cleanly") {
    BumpAllocator allocator(65536);

    SUBCASE("Transitive [move] capture (deferred to follow-up)") {
        // [move c] in a nested lambda would force the outer to also move c,
        // leaving it unable to use c. That coordination isn't implemented yet.
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun main() {
                var c: uniq Counter = uniq Counter { value = 7 };
                var f = fun(): fun() -> i32 {
                    return fun[move c](): i32 => c.value;
                };
            }
        )";
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }
}

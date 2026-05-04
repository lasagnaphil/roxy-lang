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

    SUBCASE("Builtin native (str_concat) as function reference") {
        // Native functions get IROp::CallNative inside the trampoline body
        // instead of plain Call. Builtins like str_concat are auto-imported
        // as ImportedNative symbols.
        const char* source = R"(
            fun main() {
                var c = str_concat;
                print(c("hi-", "lo"));
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "hi-lo\n");
    }

    SUBCASE("Native function reference (print) passed to higher-order") {
        const char* source = R"(
            fun greet_via(f: fun(string), name: string) { f(name); }
            fun main() {
                greet_via(print, "hello");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\n");
    }

    SUBCASE("Generic function reference via type annotation") {
        // `identity` is a template; the surrounding `var f: fun(i32)->i32`
        // annotation drives inference, which monomorphizes to identity$i32
        // and synthesizes a trampoline targeting it.
        const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun main() {
                var f: fun(i32) -> i32 = identity;
                print(f"{f(42)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Generic function reference via call-arg inference") {
        // `apply`'s param type drives the inference at the reference site.
        const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun apply(f: fun(i32) -> i32, x: i32): i32 { return f(x); }
            fun main() {
                print(f"{apply(identity, 21)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("Same generic template referenced at two type instantiations") {
        // Two distinct expected function types ⇒ two monomorphized
        // instances + two trampolines, both correct.
        const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun main() {
                var i: fun(i32) -> i32 = identity;
                var f: fun(f64) -> f64 = identity;
                print(f"{i(7)}");
                print(f"{f(3.5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "7\n3.5\n");
    }

    SUBCASE("Generic function reference without type context is rejected") {
        // No annotation, no call-arg context ⇒ inference can't bind T;
        // user gets an actionable error instead of a silent default.
        const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun main() {
                var f = identity;
                f(1);
            }
        )";
        BumpAllocator allocator(16384);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
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

TEST_CASE("E2E - Closure: edge cases") {
    SUBCASE("Closure inside for-loop captures fresh i each iteration") {
        // Each iteration creates a new closure whose `i` capture is the
        // current loop value (by-value semantic — JS-style "all closures
        // share the final i" trap doesn't happen here).
        const char* source = R"(
            fun main() {
                var fs: List<fun() -> i32> = List<fun() -> i32>();
                for (var i: i32 = 0; i < 5; i = i + 1) {
                    fs.push(fun(): i32 => i);
                }
                var result: i32 = 0;
                for (var j: i32 = 0; j < 5; j = j + 1) {
                    result = result + fs[j]();
                }
                print(f"{result}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n");  // 0+1+2+3+4
    }

    SUBCASE("Closure stored in struct field (callable via field access)") {
        // `obj.field()` where field is `fun(...)` is an indirect call, not a
        // method dispatch. (Regression: was previously mangled as a method
        // and failed at lowering.)
        const char* source = R"(
            struct Holder {
                callback: fun(i32) -> i32;
            }
            fun main() {
                var h: uniq Holder = uniq Holder {
                    callback = fun(x: i32): i32 => x + 1
                };
                print(f"{h.callback(41)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Closure stored in struct field with captured locals") {
        const char* source = R"(
            struct Adder {
                op: fun(i32) -> i32;
            }
            fun main() {
                var n: i32 = 100;
                var a: uniq Adder = uniq Adder {
                    op = fun(x: i32): i32 => x + n
                };
                print(f"{a.op(5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "105\n");
    }

    SUBCASE("Closures stored in List, indexed and called") {
        const char* source = R"(
            fun main() {
                var fs: List<fun(i32) -> i32> = List<fun(i32) -> i32>();
                fs.push(fun(x: i32): i32 => x + 1);
                fs.push(fun(x: i32): i32 => x * 2);
                print(f"{fs[0](10)} {fs[1](10)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "11 20\n");
    }

    SUBCASE("Closure capturing other closures via [move]") {
        // `combine` consumes both `inc` and `dbl` and threads them through.
        const char* source = R"(
            fun main() {
                var inc = fun(x: i32): i32 => x + 1;
                var dbl = fun(x: i32): i32 => x * 2;
                var combine = fun[move inc, move dbl](x: i32): i32 => dbl(inc(x));
                print(f"{combine(5)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "12\n");  // (5+1)*2
    }

    SUBCASE("Closure mutates shared state via captured ref") {
        // Captured `r: ref Counter` aliases `c`; the closure can mutate the
        // underlying object and the change is visible to the caller.
        const char* source = R"(
            struct Counter {
                value: i32 = 0;
            }
            fun delete Counter() {}
            fun main() {
                var c: uniq Counter = uniq Counter { value = 0 };
                var r: ref Counter = c;
                var bump = fun(): i32 {
                    r.value = r.value + 1;
                    return r.value;
                };
                print(f"{bump()}");
                print(f"{bump()}");
                print(f"{c.value}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n2\n");
    }

    SUBCASE("Closure with block body, locals, and explicit return") {
        const char* source = R"(
            fun main() {
                var f = fun(x: i32): i32 {
                    var doubled: i32 = x * 2;
                    var plus_one: i32 = doubled + 1;
                    return plus_one;
                };
                print(f"{f(10)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("Closure body uses try/catch") {
        const char* source = R"(
            struct OutOfBounds {
                idx: i32;
            }
            fun OutOfBounds.message(): string for Exception {
                return f"oob: {self.idx}";
            }
            fun main() {
                var f = fun(idx: i32): i32 {
                    try {
                        if (idx < 0) {
                            throw OutOfBounds { idx = idx };
                        }
                        return idx * 10;
                    } catch (e: OutOfBounds) {
                        return -1;
                    }
                };
                print(f"{f(5)}");
                print(f"{f(-3)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n-1\n");
    }

    SUBCASE("Reassigning a closure variable") {
        const char* source = R"(
            fun main() {
                var f = fun(x: i32): i32 => x + 1;
                print(f"{f(10)}");
                f = fun(x: i32): i32 => x * 100;
                print(f"{f(10)}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "11\n1000\n");
    }

    SUBCASE("Self capture mixed with ordinary parameter capture") {
        const char* source = R"(
            struct S {
                base: i32 = 0;
            }
            fun delete S() {}
            fun S.bump_by(amount: i32): fun() -> i32 {
                return fun(): i32 => self.base + amount;
            }
            fun main() {
                var s: uniq S = uniq S { base = 100 };
                var f = s.bump_by(7);
                print(f"{f()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "107\n");
    }

    SUBCASE("Self capture calls inherited method") {
        // Closure captures self; body invokes a method defined on the parent
        // struct — exercises method dispatch through the captured ref.
        const char* source = R"(
            struct Base {
                x: i32 = 0;
            }
            fun delete Base() {}
            fun Base.value(): i32 { return self.x; }

            struct Derived : Base {}
            fun delete Derived() {}

            fun Derived.make_getter(): fun() -> i32 {
                return fun(): i32 => self.value();
            }

            fun main() {
                var d: uniq Derived = uniq Derived { x = 7 };
                var g = d.make_getter();
                print(f"{g()}");
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "7\n");
    }

    SUBCASE("Void no-op closure and void closure that captures a string") {
        const char* source = R"(
            fun main() {
                var msg: string = "hello";
                var noop = fun() {};
                var greet = fun() {
                    print(msg);
                };
                noop();
                greet();
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\n");
    }
}

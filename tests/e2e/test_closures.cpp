#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Closures
// ============================================================================

TEST_SUITE("E2E Closures") {

    TEST_CASE_TEMPLATE("lambda creation and immediate call", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Single arg, expression body") {
            const char* source = R"(
            fun main() {
                var f = fun(x: i32): i32 => x + 1;
                print(f"{f(5)}");
            }
        )";
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "hello world\n");
        }
    }

    TEST_CASE_TEMPLATE("higher-order functions", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Pass closure as parameter") {
            const char* source = R"(
            fun apply(f: fun(i32) -> i32, x: i32): i32 {
                return f(x);
            }
            fun main() {
                print(f"{apply(fun(x: i32): i32 => x + 1, 5)}");
            }
        )";
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "100\n");
        }
    }

    TEST_CASE_TEMPLATE("implicit copy capture", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Capture i32 by value") {
            const char* source = R"(
            fun main() {
                var n: i32 = 10;
                var f = fun(): i32 => n + 1;
                print(f"{f()}");
            }
        )";
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "8\n");
        }
    }

    TEST_CASE_TEMPLATE("explicit move capture", Backend, RX_E2E_BACKENDS) {
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "42\n");
        }

        SUBCASE("Closure frees its moved-in capture when it drops") {
            // The closure env owns the moved-in Counter; when the closure drops,
            // its synthesized env destructor (dispatched by the env's type_id)
            // must destroy it exactly once. `r` is moved into `f`, so main does
            // not also free it.
            const char* source = R"(
            struct Counter { value: i32 = 0; }
            fun delete Counter() { print(f"del {self.value}"); }
            fun main(): i32 {
                var r: uniq Counter = uniq Counter { value = 7 };
                {
                    var f = fun[move r](): i32 => r.value;
                    print(f"{f()}");
                }
                print("after");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "7\ndel 7\nafter\n");
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

    TEST_CASE("capture rule errors") {
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

    TEST_CASE_TEMPLATE("function references", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Bare function name to typed variable") {
            const char* source = R"(
            fun double(x: i32): i32 { return x * 2; }
            fun main() {
                var f: fun(i32) -> i32 = double;
                print(f"{f(21)}");
            }
        )";
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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

        SUBCASE("Explicit type-args in value position: identity<i32>") {
            // Parser commits the type-args list because `;` is in the safe
            // follow-token set. No inference needed; semantic instantiates
            // immediately with the explicit i32.
            const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun main() {
                var f = identity<i32>;
                print(f"{f(42)}");
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "42\n");
        }

        SUBCASE("Explicit type-args passed as call argument") {
            const char* source = R"(
            fun identity<T>(value: T): T { return value; }
            fun apply(f: fun(i32) -> i32, x: i32): i32 { return f(x); }
            fun main() {
                print(f"{apply(identity<i32>, 21)}");
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "21\n");
        }

        SUBCASE("Comparison still parses correctly with new commit set") {
            // `a < b, c > x` inside call-args must still parse as TWO comparison
            // expressions, not a generic ref. The trial parse is rejected because
            // `>` is followed by `x` (an identifier), not a commit-token.
            const char* source = R"(
            fun choose(a: bool, b: bool): bool { return a || b; }
            fun main() {
                var a: i32 = 1;
                var b: i32 = 2;
                var c: i32 = 3;
                var x: i32 = 5;
                if (choose(a < b, c > x)) {
                    print("ok");
                } else {
                    print("nope");
                }
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "ok\n");
        }

        SUBCASE("Struct template in value position rejected with clear error") {
            // Box<i32> followed by `;` would commit type-args; semantic rejects
            // with a message that names the struct rather than the generic
            // "undefined identifier".
            const char* source = R"(
            struct Box<T> { value: T; }
            fun main() {
                var x = Box<i32>;
            }
        )";
            BumpAllocator allocator(16384);
            BCModule* module = compile(allocator, source);
            CHECK(module == nullptr);
        }
    }

    TEST_CASE_TEMPLATE("nested closures", Backend, RX_E2E_BACKENDS) {
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "21\n");
        }
    }

    TEST_CASE("self capture") {  // VM-only: C backend: self-capture / function-to-borrow conversion gap
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
            CHECK_FALSE(result.success);
        }
    }

    TEST_CASE("self capture compile-time rejections") {
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

    TEST_CASE("nested self capture") {  // VM-only: C backend: self-capture / function-to-borrow conversion gap
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
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
            auto result = VMBackend::run(source);
            CHECK_FALSE(result.success);
        }
    }

    TEST_CASE("transitive [move] across nested lambdas") {  // VM-only: C backend: self-capture / function-to-borrow conversion gap
        SUBCASE("Inner [move c] from a noncopyable across one outer lambda") {
            // `c` lives in main's scope; the outer lambda doesn't reference it
            // directly, but the inner lambda's [move c] propagates a Move
            // capture through the outer so ownership flows: main → outer.env
            // → inner.env.
            const char* source = R"(
            struct Counter { value: i32 = 0; }
            fun delete Counter() {}
            fun main() {
                var c: uniq Counter = uniq Counter { value = 7 };
                var make = fun(): fun() -> i32 {
                    return fun[move c](): i32 => c.value;
                };
                var inner = make();
                print(f"{inner()}");
            }
        )";
            auto result = VMBackend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "7\n");
        }

        SUBCASE("Use-after-move in the enclosing scope is still rejected") {
            const char* source = R"(
            struct Counter { value: i32 = 0; }
            fun delete Counter() {}
            fun main() {
                var c: uniq Counter = uniq Counter { value = 7 };
                var make = fun(): fun() -> i32 {
                    return fun[move c](): i32 => c.value;
                };
                var inner = make();
                var leak: i32 = c.value;  // use-after-move
            }
        )";
            BumpAllocator allocator(65536);
            BCModule* module = compile(allocator, source);
            CHECK(module == nullptr);
        }

        SUBCASE("Transitive [move] across two outer lambdas") {
            const char* source = R"(
            struct Counter { value: i32 = 0; }
            fun delete Counter() {}
            fun main() {
                var c: uniq Counter = uniq Counter { value = 99 };
                var lvl0 = fun(): fun() -> fun() -> i32 {
                    return fun(): fun() -> i32 {
                        return fun[move c](): i32 => c.value;
                    };
                };
                var lvl1 = lvl0();
                var inner = lvl1();
                print(f"{inner()}");
            }
        )";
            auto result = VMBackend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "99\n");
        }
    }

    TEST_CASE_TEMPLATE("edge cases", Backend, RX_E2E_BACKENDS) {
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
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
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "hello\n");
        }
    }

    TEST_CASE("function-to-borrow conversion") {  // VM-only: C backend: self-capture / function-to-borrow conversion gap
        SUBCASE("pass a fun to a ref fun parameter and call it") {
            // `fun -> ref fun` borrows the closure (like uniq -> ref); the borrow
            // is callable, and the caller's `f` stays usable afterward.
            const char* source = R"(
            fun apply(rf: ref fun(i32) -> i32, x: i32): i32 {
                return rf(x);
            }
            fun main(): i32 {
                var f = fun(x: i32): i32 => x + 1;
                var a: i32 = apply(f, 41);   // f borrowed as ref fun
                var b: i32 = apply(f, 9);    // f still usable
                return a + b;                // 42 + 10 == 52
            }
        )";
            auto result = VMBackend::run(source);
            CHECK(result.success);
            CHECK(result.value == 52);
        }

        SUBCASE("pass a fun to a weak fun parameter and call it") {
            // `fun -> weak fun` captures the env generation via WeakCreate.
            const char* source = R"(
            fun apply(wf: weak fun(i32) -> i32, x: i32): i32 {
                return wf(x);
            }
            fun main(): i32 {
                var f = fun(x: i32): i32 => x * 3;
                return apply(f, 14);   // 42
            }
        )";
            auto result = VMBackend::run(source);
            CHECK(result.success);
            CHECK(result.value == 42);
        }
    }

    TEST_CASE_TEMPLATE("borrowed function values are callable", Backend, RX_E2E_BACKENDS) {
        SUBCASE("borrow a function out of a list and call it") {
            // `List<fun>.index` returns `borrowed fun` == `ref fun`, so a bound
            // element is a borrow of the list's closure (no double-free) and is
            // still callable.
            const char* source = R"(
            fun main(): i32 {
                var fs: List<fun(i32) -> i32> = List<fun(i32) -> i32>();
                fs.push(fun(x: i32): i32 => x + 1);
                fs.push(fun(x: i32): i32 => x * 2);
                var g: ref fun(i32) -> i32 = fs[1];   // borrow, not move
                return fs[0](10) + g(10);             // 11 + 20 == 31
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.value == 31);
        }

        SUBCASE("moving a function out of a list is rejected") {
            // Binding to an owning `fun` would move the closure out from under the
            // list (double-free). Since index yields `ref fun`, this is a plain
            // ref->fun type error.
            const char* source = R"(
            fun main(): i32 {
                var fs: List<fun(i32) -> i32> = List<fun(i32) -> i32>();
                fs.push(fun(x: i32): i32 => x + 1);
                var moved: fun(i32) -> i32 = fs[0];   // ref fun -> fun: type error
                return moved(10);
            }
        )";
            BumpAllocator allocator(65536);
            BCModule* module = compile(allocator, source);
            CHECK(module == nullptr);  // Should fail to compile
        }
    }

}  // TEST_SUITE("E2E Closures")

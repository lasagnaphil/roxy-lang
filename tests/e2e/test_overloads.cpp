#include "roxy/core/doctest/doctest.h"
#include "test_e2e_backend.hpp"
#include "test_helpers.hpp"

#include "roxy/compiler/driver/compiler.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/vm.hpp"

#include <cstring>

using namespace rx;

// ============================================================================
// Function overloading: user-facing overload sets, per-type print overloads,
// and the print Printable fallback. See docs/internals/overloading.md.
// ============================================================================

TEST_SUITE("E2E Overloads") {

    TEST_CASE_TEMPLATE("Dispatch by parameter type and arity", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun describe(x: i32): string { return f"int {x}"; }
        fun describe(s: string): string { return f"str {s}"; }
        fun describe(x: i32, y: i32): string { return f"pair {x} {y}"; }

        fun main(): i32 {
            print(describe(42));
            print(describe("hi"));
            print(describe(1, 2));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "int 42\nstr hi\npair 1 2\n");
    }

    TEST_CASE_TEMPLATE("Overloads calling each other and themselves", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i32): i32 {
            if (x <= 0) { return 0; }
            return f(x - 1) + 1;
        }
        fun f(s: string): i32 { return f(3); }

        fun main(): i32 {
            print(f"{f(5)}");
            print(f"{f("x")}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "5\n3\n");
    }

    TEST_CASE_TEMPLATE("Non-pub overloads", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun double_it(x: i32): i32 { return x * 2; }
        fun double_it(x: f64): f64 { return x * 2.0; }

        fun main(): i32 {
            print(f"{double_it(21)}");
            print(f"{double_it(1.25)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n2.5\n");
    }

    TEST_CASE_TEMPLATE("Overload with out and inout params", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun get(x: out i32) { x = 10; }
        fun get(x: out i32, y: out i32) { x = 1; y = 2; }
        fun bump(v: inout i32) { v = v + 1; }
        fun bump(v: inout i32, by: i32) { v = v + by; }

        fun main(): i32 {
            var a: i32 = 0;
            get(out a);
            print(f"{a}");
            var b: i32 = 0;
            var c: i32 = 0;
            get(out b, out c);
            print(f"{b + c}");
            bump(inout a);
            print(f"{a}");
            bump(inout a, 9);
            print(f"{a}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n3\n11\n20\n");
    }

    TEST_CASE_TEMPLATE("Literal arguments pick the settled default", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun which(x: i32): string { return "i32"; }
        fun which(x: i64): string { return "i64"; }
        fun which(x: f64): string { return "f64"; }
        fun which(x: f32): string { return "f32"; }

        fun main(): i32 {
            print(which(42));         // unsuffixed int -> i32
            print(which(42l));        // suffixed -> i64
            print(which(3.14));       // unsuffixed float -> f64
            print(which(3.14f));      // suffixed -> f32
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "i32\ni64\nf64\nf32\n");
    }

    TEST_CASE_TEMPLATE("Function reference disambiguated by context", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i32): i32 { return x * 2; }
        fun f(s: string): string { return s; }

        fun apply(g: fun(i32) -> i32, v: i32): i32 { return g(v); }

        fun main(): i32 {
            var g: fun(i32) -> i32 = f;   // typed var picks the i32 member
            print(f"{g(21)}");
            print(f"{apply(f, 10)}");     // call-arg context picks it too
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n20\n");
    }

    TEST_CASE_TEMPLATE("Noncopyable argument moves into the chosen overload", Backend,
                       RX_E2E_BACKENDS) {
        const char* source = R"(
        fun consume(xs: List<i32>): i32 { return xs.len(); }
        fun consume(xs: List<i32>, extra: i32): i32 { return xs.len() + extra; }

        fun main(): i32 {
            var a: List<i32> = List<i32>();
            a.push(1); a.push(2);
            print(f"{consume(a)}");
            var b: List<i32> = List<i32>();
            b.push(1);
            print(f"{consume(b, 10)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n11\n");
    }

    // ------------------------------------------------------------------------
    // print overloads + Printable fallback
    // ------------------------------------------------------------------------

    TEST_CASE_TEMPLATE("print overloads for every Printable primitive", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print("hello");
            print(42);
            print(9999999999l);
            print(true);
            print(1.5);
            var u: u32 = 4000000000u;
            print(u);
            var b: u64 = 18446744073709551615ul;
            print(b);
            var f: f32 = 2.5f;
            print(f);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output ==
              "hello\n42\n9999999999\ntrue\n1.5\n4000000000\n18446744073709551615\n2.5\n");
    }

    TEST_CASE_TEMPLATE("print Printable fallback: struct, enum, containers", Backend,
                       RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Vec { x: i32; }
        fun Vec.to_string(): string for Printable { return f"Vec[{self.x}]"; }

        enum Color { Red, Green, Blue }

        fun main(): i32 {
            print(Vec { x = 7 });
            print(Color::Blue);
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2); xs.push(3);
            print(xs);
            var m: Map<string, i32> = Map<string, i32>();
            m.insert("potion", 3);
            print(m);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "Vec[7]\n2\n[1, 2, 3]\n{potion: 3}\n");
    }

    TEST_CASE_TEMPLATE("print passed as fun(string) value keeps working", Backend,
                       RX_E2E_BACKENDS) {
        const char* source = R"(
        fun greet_via(f: fun(string), name: string) { f(name); }
        fun main(): i32 {
            greet_via(print, "hello");
            var p: fun(string) = print;
            p("typed");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\ntyped\n");
    }

    // ------------------------------------------------------------------------
    // Cross-module overload sets
    // ------------------------------------------------------------------------

    TEST_CASE("Cross-module: from-imported overload set (VM)") {
        // Multi-file compile via the Compiler API (pattern from test_modules).
        const char* util_source = R"(
        pub fun pick(x: i32): i32 { return 1; }
        pub fun pick(s: string): i32 { return 2; }
    )";
        const char* main_source = R"(
        from util import pick;

        fun main(): i32 {
            return pick(7) * 10 + pick("seven");
        }
    )";

        BumpAllocator allocator(16384);
        Compiler compiler(allocator);
        compiler.add_source("util", util_source, static_cast<u32>(strlen(util_source)));
        compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

        BCModule* module = compiler.compile();
        REQUIRE(module != nullptr);

        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, module);
        REQUIRE(vm_call(&vm, "main", {}));
        Value result = vm_get_result(&vm);
        CHECK(result.as_int == 12);
        vm_destroy(&vm);
        delete module;
    }

    // ------------------------------------------------------------------------
    // Negative cases (diagnostics)
    // ------------------------------------------------------------------------

    TEST_CASE_TEMPLATE("Duplicate signature is a redefinition error", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i32): i32 { return 1; }
        fun f(x: i32): string { return "x"; }
        fun main(): i32 { return 0; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Ambiguous call is rejected", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i64): i32 { return 1; }
        fun f(x: f32): i32 { return 2; }
        fun main(): i32 {
            f(42);   // settles i32: no exact match, both assignable
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("main cannot be overloaded", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 { return 0; }
        fun main(x: i32): i32 { return x; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic and concrete definitions are exclusive", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f<T>(v: T): T { return v; }
        fun f(x: i32): i32 { return x; }
        fun main(): i32 { return 0; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Ambiguous bare reference is rejected", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i32): i32 { return 1; }
        fun f(s: string): i32 { return 2; }
        fun main(): i32 {
            var g = f;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("No matching overload lists candidates", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun f(x: i32): i32 { return 1; }
        fun f(s: string): i32 { return 2; }
        fun main(): i32 {
            f(true);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

} // TEST_SUITE("E2E Overloads")

#include "roxy/core/doctest/doctest.h"
#include "test_e2e_backend.hpp"
#include "test_helpers.hpp"

#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/vm.hpp"

using namespace rx;

// ============================================================================
// Method Tests
// ============================================================================

TEST_SUITE("E2E Methods") {

    TEST_CASE_TEMPLATE("Basic method call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.sum(): i32 {
            return self.x + self.y;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            print(f"{p.sum()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    TEST_CASE_TEMPLATE("Method with parameters", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.add(dx: i32, dy: i32): i32 {
            return self.x + dx + self.y + dy;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            print(f"{p.add(5, 15)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n");
    }

    TEST_CASE_TEMPLATE("Method modifying self", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun Counter.increment() {
            self.value = self.value + 1;
        }

        fun Counter.add(n: i32) {
            self.value = self.value + n;
        }

        fun main(): i32 {
            var c: Counter = Counter { value = 0 };
            c.increment();
            print(f"{c.value}");
            c.add(10);
            print(f"{c.value}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n11\n");
    }

    TEST_CASE_TEMPLATE("Method returning struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.scaled(factor: i32): Point {
            return Point { x = self.x * factor, y = self.y * factor };
        }

        fun main(): i32 {
            var p: Point = Point { x = 3, y = 4 };
            var q: Point = p.scaled(2);
            print(f"{q.x}");
            print(f"{q.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "6\n8\n");
    }

    TEST_CASE_TEMPLATE("Multiple methods on same struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Rect {
            width: i32;
            height: i32;
        }

        fun Rect.area(): i32 {
            return self.width * self.height;
        }

        fun Rect.perimeter(): i32 {
            return 2 * (self.width + self.height);
        }

        fun main(): i32 {
            var r: Rect = Rect { width = 5, height = 3 };
            print(f"{r.area()}");
            print(f"{r.perimeter()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "15\n16\n");
    }

    TEST_CASE_TEMPLATE("Method on heap-allocated struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.sum(): i32 {
            return self.x + self.y;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point { x = 100, y = 200 };
            print(f"{p.sum()}");
            delete p;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "300\n");
    }

    TEST_CASE_TEMPLATE("Chained method calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun Counter.get(): i32 {
            return self.value;
        }

        fun main(): i32 {
            var c1: Counter = Counter { value = 5 };
            var c2: Counter = Counter { value = 10 };
            print(f"{c1.get()}");
            print(f"{c2.get()}");
            print(f"{c1.get() + c2.get()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "5\n10\n15\n");
    }

    TEST_CASE_TEMPLATE("Method with struct parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.distance_sq(other: Point): i32 {
            var dx: i32 = self.x - other.x;
            var dy: i32 = self.y - other.y;
            return dx * dx + dy * dy;
        }

        fun main(): i32 {
            var p1: Point = Point { x = 0, y = 0 };
            var p2: Point = Point { x = 3, y = 4 };
            print(f"{p1.distance_sq(p2)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "25\n");
    }

    // ------------------------------------------------------------------------
    // Primitive/enum receiver method dispatch: builtin trait methods
    // (to_string/hash) call their registered natives; operator-named methods
    // (eq/lt/div/...) lower to the same raw IR ops as the operator expression.
    // ------------------------------------------------------------------------

    TEST_CASE_TEMPLATE("Primitive to_string method calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"{42.to_string()}");
            var x: i32 = 7;
            print(f"{x.to_string()}");
            print(f"{(3.5).to_string()}");
            print(f"{true.to_string()}");
            print(f"{"abc".to_string()}");
            var big: i64 = 9999999999l;
            print(f"{big.to_string()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n7\n3.5\ntrue\nabc\n9999999999\n");
    }

    TEST_CASE_TEMPLATE("Primitive hash method call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 7;
            var h1: u64 = x.hash();
            var h2: u64 = x.hash();
            // Same input hashes equal; hash is avalanched so 7 won't map to 0.
            print(f"{h1 == h2}");
            print(f"{h1 != 0ul}");
            var s: string = "hello";
            print(f"{s.hash() == s.hash()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\ntrue\ntrue\n");
    }

    TEST_CASE_TEMPLATE("Enum to_string method call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Color { Red, Green, Blue }

        fun main(): i32 {
            var c: Color = Color::Green;
            print(f"{c.to_string()}");
            print(f"{Color::Blue.to_string()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n");
    }

    TEST_CASE_TEMPLATE("Operator-named methods on primitives", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"{(1).eq(1)}");
            print(f"{(1).ne(2)}");
            print(f"{(1).lt(2)}");
            print(f"{(2).ge(2)}");
            print(f"{(10).add(5)}");
            print(f"{(10).sub(3)}");
            print(f"{(6).mul(7)}");
            print(f"{(7).neg()}");
            print(f"{(1.5).lt(2.5)}");
            print(f"{"a".eq("a")}");
            print(f"{"a".ne("b")}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\ntrue\ntrue\ntrue\n15\n7\n42\n-7\ntrue\ntrue\ntrue\n");
    }

    TEST_CASE_TEMPLATE("Operator-named methods use unsigned semantics", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            // > 2^63: a signed compare would say a < b.
            var a: u64 = 18446744073709551615ul;
            var b: u64 = 5ul;
            print(f"{b.lt(a)}");
            print(f"{a.gt(b)}");
            // > 2^31: a signed div would go negative.
            var u: u32 = 4000000000u;
            print(f"{u.div(2u)}");
            print(f"{u.mod(3u)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\ntrue\n2000000000\n1\n");
    }

} // TEST_SUITE("E2E Methods")

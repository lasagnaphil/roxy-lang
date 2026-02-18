#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Method Tests
// ============================================================================

TEST_CASE("E2E - Basic method call") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");
}

TEST_CASE("E2E - Method with parameters") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "50\n");
}

TEST_CASE("E2E - Method modifying self") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n11\n");
}

TEST_CASE("E2E - Method returning struct") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "6\n8\n");
}

TEST_CASE("E2E - Multiple methods on same struct") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "15\n16\n");
}

TEST_CASE("E2E - Method on heap-allocated struct") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "300\n");
}

TEST_CASE("E2E - Chained method calls") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n10\n15\n");
}

TEST_CASE("E2E - Method with struct parameter") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "25\n");
}

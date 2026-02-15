#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Generic Function Tests
// ============================================================================

TEST_CASE("E2E - Generic function identity i32") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function identity f64") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: f64 = identity<f64>(3.14);
            return i32(x);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Generic function identity bool") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: bool = identity<bool>(true);
            if (x) { return 1; }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Generic function multiple instantiations") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var a: i32 = identity<i32>(10);
            var b: i32 = identity<i32>(20);
            var c: f64 = identity<f64>(1.5);
            return a + b + i32(c);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 31);
}

TEST_CASE("E2E - Generic function two type params") {
    const char* source = R"(
        fun first<T, U>(a: T, b: U): T {
            return a;
        }

        fun main(): i32 {
            return first<i32, f64>(42, 3.14);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function second of two params") {
    const char* source = R"(
        fun second<T, U>(a: T, b: U): U {
            return b;
        }

        fun main(): i32 {
            var x: f64 = second<i32, f64>(42, 2.5);
            return i32(x);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 2);
}

TEST_CASE("E2E - Generic function with computation") {
    const char* source = R"(
        fun double_val<T>(value: T): T {
            return value + value;
        }

        fun main(): i32 {
            return double_val<i32>(21);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function with struct type arg") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun get_x<T>(p: T): i32 {
            return 99;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            return get_x<Point>(p);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
}

// ============================================================================
// Generic Struct Tests
// ============================================================================

TEST_CASE("E2E - Generic struct literal i32") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic struct literal f64") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<f64> = Box<f64> { value = 3.14 };
            return i32(b.value);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Generic struct two type params") {
    const char* source = R"(
        struct Pair<T, U> {
            first: T;
            second: U;
        }

        fun main(): i32 {
            var p: Pair<i32, f64> = Pair<i32, f64> { first = 10, second = 2.5 };
            return p.first + i32(p.second);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 12);
}

TEST_CASE("E2E - Generic struct different instantiations") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var a: Box<i32> = Box<i32> { value = 10 };
            var b: Box<f64> = Box<f64> { value = 5.5 };
            return a.value + i32(b.value);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 15);
}

TEST_CASE("E2E - Generic struct as function parameter") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun unbox(b: Box<i32>): i32 {
            return b.value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return unbox(b);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic struct as function return type") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun make_box(v: i32): Box<i32> {
            return Box<i32> { value = v };
        }

        fun main(): i32 {
            var b: Box<i32> = make_box(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function with generic struct") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun wrap<T>(v: T): Box<T> {
            return Box<T> { value = v };
        }

        fun main(): i32 {
            var b: Box<i32> = wrap<i32>(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Out/Inout Parameter Tests
// ============================================================================

TEST_CASE("E2E - Basic inout parameter") {
    const char* source = R"(
        fun increment(x: inout i32) {
            x = x + 1;
        }

        fun main(): i32 {
            var n: i32 = 41;
            increment(inout n);
            print(n);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Basic out parameter") {
    const char* source = R"(
        fun init_value(x: out i32) {
            x = 42;
        }

        fun main(): i32 {
            var n: i32 = 0;
            init_value(out n);
            print(n);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Multiple out parameters") {
    const char* source = R"(
        fun init_pair(a: out i32, b: out i32) {
            a = 10;
            b = 20;
        }

        fun main(): i32 {
            var x: i32 = 0;
            var y: i32 = 0;
            init_pair(out x, out y);
            print(x);
            print(y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Swap with inout") {
    const char* source = R"(
        fun swap(a: inout i32, b: inout i32) {
            var temp: i32 = a;
            a = b;
            b = temp;
        }

        fun main(): i32 {
            var x: i32 = 10;
            var y: i32 = 20;
            swap(inout x, inout y);
            print(x);
            print(y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n10\n");  // swapped
}

TEST_CASE("E2E - Inout with computation") {
    const char* source = R"(
        fun double_value(x: inout i32) {
            x = x * 2;
        }

        fun main(): i32 {
            var n: i32 = 21;
            double_value(inout n);
            print(n);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");  // 21 * 2
}

TEST_CASE("E2E - Multiple inout calls") {
    const char* source = R"(
        fun increment(x: inout i32) {
            x = x + 1;
        }

        fun main(): i32 {
            var n: i32 = 0;
            increment(inout n);
            print(n);
            increment(inout n);
            print(n);
            increment(inout n);
            print(n);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

TEST_CASE("E2E - Mixed regular and inout parameters") {
    const char* source = R"(
        fun add_to(x: inout i32, amount: i32) {
            x = x + amount;
        }

        fun main(): i32 {
            var n: i32 = 10;
            add_to(inout n, 32);
            print(n);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");  // 10 + 32
}

TEST_CASE("E2E - Inout with struct parameter") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun double_point(p: inout Point) {
            p.x = p.x * 2;
            p.y = p.y * 2;
        }

        fun main(): i32 {
            var pt = new Point { x = 10, y = 20 };
            double_point(inout pt);
            print(pt.x);
            print(pt.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n40\n");  // 10*2, 20*2
}

TEST_CASE("E2E - Out with struct parameter") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun init_point(p: out Point, x: i32, y: i32) {
            p.x = x;
            p.y = y;
        }

        fun main(): i32 {
            var pt: Point;
            init_point(out pt, 15, 27);
            print(pt.x);
            print(pt.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "15\n27\n");
}

TEST_CASE("E2E - Inout struct field modification") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun swap_coords(p: inout Point) {
            var temp: i32 = p.x;
            p.x = p.y;
            p.y = temp;
        }

        fun main(): i32 {
            var pt = new Point { x = 10, y = 20 };
            swap_coords(inout pt);
            print(pt.x);
            print(pt.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n10\n");  // swapped
}

TEST_CASE("E2E - Inout with nested struct") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        struct Rect {
            origin: Point;
            size: Point;
        }

        fun scale_rect(r: inout Rect, factor: i32) {
            r.origin.x = r.origin.x * factor;
            r.origin.y = r.origin.y * factor;
            r.size.x = r.size.x * factor;
            r.size.y = r.size.y * factor;
        }

        fun main(): i32 {
            var rect = new Rect {
                origin = new Point { x = 1, y = 2 },
                size = new Point { x = 10, y = 20 }
            };
            scale_rect(inout rect, 3);
            print(rect.origin.x);
            print(rect.origin.y);
            print(rect.size.x);
            print(rect.size.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "3\n6\n30\n60\n");  // 1*3, 2*3, 10*3, 20*3
}

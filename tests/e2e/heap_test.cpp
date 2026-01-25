#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Heap Allocation Tests - Basic New/Delete
// ============================================================================

TEST_CASE("E2E - Heap allocation basic") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 10;
            p.y = 20;
            var result: i32 = p.x + p.y;
            delete p;
            return result;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.value == 30);
}

TEST_CASE("E2E - Heap allocation with print") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 42;
            p.y = 58;
            print(p.x);
            print(p.y);
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n58\n");
}

TEST_CASE("E2E - Multiple heap allocations") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p1: uniq Point = new Point();
            var p2: uniq Point = new Point();
            p1.x = 1;
            p1.y = 2;
            p2.x = 10;
            p2.y = 20;
            print(p1.x);
            print(p2.x);
            delete p1;
            delete p2;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n10\n");
}

TEST_CASE("E2E - Heap allocation larger struct") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i32;
            c: i32;
            d: i64;
        }

        fun main(): i32 {
            var d: uniq Data = new Data();
            d.a = 1;
            d.b = 2;
            d.c = 3;
            d.d = 100000000000;
            print(d.a);
            print(d.b);
            print(d.c);
            print(d.d);
            delete d;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n100000000000\n");
}

TEST_CASE("E2E - Heap allocation with computation") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 5;
            p.y = 7;
            var sum: i32 = p.x + p.y;
            var product: i32 = p.x * p.y;
            print(sum);
            print(product);
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "12\n35\n");
}

TEST_CASE("E2E - Heap allocation in loop") {
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun main(): i32 {
            var sum: i32 = 0;
            for (var i: i32 = 0; i < 3; i = i + 1) {
                var c: uniq Counter = new Counter();
                c.value = i * 10;
                sum = sum + c.value;
                delete c;
            }
            print(sum);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");  // 0 + 10 + 20
}

// ============================================================================
// Heap Allocation Tests - Constraint Reference Model
// ============================================================================

TEST_CASE("E2E - Uniq passed to function") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun sum_point(p: uniq Point): i32 {
            return p.x + p.y;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 10;
            p.y = 20;
            var result: i32 = sum_point(p);
            print(result);
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");
}

TEST_CASE("E2E - Heap allocation nested struct") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        struct Line {
            start: Point;
            end: Point;
        }

        fun main(): i32 {
            var line: uniq Line = new Line();
            line.start.x = 0;
            line.start.y = 0;
            line.end.x = 10;
            line.end.y = 10;
            print(line.start.x);
            print(line.end.x);
            delete line;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n10\n");
}

// ============================================================================
// Borrow Checking Tests (Constraint Reference Model)
// ============================================================================

TEST_CASE("E2E - Constraint reference borrow check success") {
    // Test that delete with no active borrows succeeds
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun read_point(p: uniq Point): i32 {
            return p.x + p.y;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 42;
            p.y = 8;
            var result: i32 = read_point(p);
            delete p;
            print(result);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "50\n");
}

TEST_CASE("E2E - Delete null pointer is safe") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = nil;
            delete p;
            print(42);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

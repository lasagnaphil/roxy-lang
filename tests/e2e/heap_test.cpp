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

TEST_CASE("E2E - Ref parameter borrow tracking") {
    // Test that ref parameters properly track borrows via RefInc/RefDec
    // The ref parameter increments ref_count at entry, decrements at exit
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun borrow_point(p: ref Point): i32 {
            // p is a borrowed reference - ref_count was incremented at entry
            return p.x + p.y;
            // ref_count is decremented before return
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 10;
            p.y = 25;
            var result: i32 = borrow_point(p);  // Pass uniq as ref
            // After function returns, ref_count is back to 0
            delete p;  // Should succeed - no active borrows
            print(result);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "35\n");
}

TEST_CASE("E2E - Multiple ref borrows in sequence") {
    // Test multiple sequential borrows
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun get_x(p: ref Point): i32 {
            return p.x;
        }

        fun get_y(p: ref Point): i32 {
            return p.y;
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 100;
            p.y = 200;
            var x: i32 = get_x(p);  // Borrow 1
            var y: i32 = get_y(p);  // Borrow 2
            delete p;
            print(x);
            print(y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n200\n");
}

TEST_CASE("E2E - Ref parameter with multiple returns") {
    // Test that RefDec is emitted on all return paths
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun max_coord(p: ref Point): i32 {
            if (p.x > p.y) {
                return p.x;  // RefDec before this return
            }
            return p.y;  // RefDec before this return too
        }

        fun main(): i32 {
            var p: uniq Point = new Point();
            p.x = 50;
            p.y = 30;
            var m: i32 = max_coord(p);
            delete p;
            print(m);
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

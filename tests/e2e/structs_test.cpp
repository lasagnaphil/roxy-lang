#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Struct Tests
// ============================================================================

TEST_CASE("E2E - Basic struct field access") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 10;
            p.y = 20;
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Struct with 64-bit field") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i64;
        }

        fun main(): i32 {
            var d: Data;
            d.a = 10;
            d.b = 100000000000l;
            print(d.a);
            print_i64(d.b);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n100000000000\n");
}

TEST_CASE("E2E - Multiple struct variables") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p1: Point;
            var p2: Point;
            p1.x = 1;
            p1.y = 2;
            p2.x = 3;
            p2.y = 4;
            print(p1.x);
            print(p1.y);
            print(p2.x);
            print(p2.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n");
}

TEST_CASE("E2E - Struct field assignment from expression") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 3 * 4;
            p.y = p.x + 5;
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "12\n17\n");
}

TEST_CASE("E2E - Struct with float fields") {
    const char* source = R"(
        struct Vec2 {
            x: f64;
            y: f64;
        }

        fun main(): f64 {
            var v: Vec2;
            v.x = 1.5;
            v.y = 2.5;
            return v.x + v.y;
        }
    )";

    Value result = compile_and_run(source, "main");
    Value float_result = Value::float_from_u64(result.as_u64());
    CHECK(float_result.as_float == doctest::Approx(4.0));  // 1.5 + 2.5
}

TEST_CASE("E2E - Struct in conditional") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 10;
            p.y = 5;
            if (p.x > p.y) {
                print(p.x);
            } else {
                print(p.y);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n");
}

TEST_CASE("E2E - Struct in loop") {
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun main(): i32 {
            var c: Counter;
            c.value = 0;
            for (var i: i32 = 0; i < 5; i = i + 1) {
                c.value = c.value + i;
                print(c.value);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n1\n3\n6\n10\n");  // cumulative: 0, 0+1, 1+2, 3+3, 6+4
}

TEST_CASE("E2E - Nested structs") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        struct Rect {
            origin: Point;
            size: Point;
        }

        fun main(): i32 {
            var r: Rect;
            r.origin.x = 10;
            r.origin.y = 20;
            r.size.x = 100;
            r.size.y = 200;
            print(r.origin.x);
            print(r.origin.y);
            print(r.size.x);
            print(r.size.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n100\n200\n");
}

TEST_CASE("E2E - Deeply nested structs (3 levels)") {
    const char* source = R"(
        struct Inner {
            value: i32;
        }

        struct Middle {
            inner: Inner;
            data: i32;
        }

        struct Outer {
            middle: Middle;
            id: i32;
        }

        fun main(): i32 {
            var o: Outer;
            o.id = 1;
            o.middle.data = 10;
            o.middle.inner.value = 100;
            print(o.id);
            print(o.middle.data);
            print(o.middle.inner.value);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n10\n100\n");
}

TEST_CASE("E2E - Multiple nested struct variables") {
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
            var line1: Line;
            var line2: Line;

            line1.start.x = 0;
            line1.start.y = 0;
            line1.end.x = 10;
            line1.end.y = 10;

            line2.start.x = 5;
            line2.start.y = 5;
            line2.end.x = 15;
            line2.end.y = 15;

            print(line1.end.x);
            print(line2.start.x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n5\n");
}

// ============================================================================
// Struct Literal Tests
// ============================================================================

TEST_CASE("E2E - Struct literal") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p = Point { x = 10, y = 20 };
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Struct literal with default values") {
    const char* source = R"(
        struct Config {
            width: i32 = 800;
            height: i32 = 600;
            fullscreen: i32 = 0;
        }

        fun main(): i32 {
            var c = Config { width = 1920 };
            print(c.width);
            print(c.height);
            print(c.fullscreen);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1920\n600\n0\n");
}

TEST_CASE("E2E - Struct literal field order") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p = Point { y = 20, x = 10 };
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Empty struct literal (all defaults)") {
    const char* source = R"(
        struct Defaults {
            a: i32 = 1;
            b: i32 = 2;
            c: i32 = 3;
        }

        fun main(): i32 {
            var d = Defaults {};
            print(d.a);
            print(d.b);
            print(d.c);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

TEST_CASE("E2E - Struct literal with expressions") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var a = 5;
            var p = Point { x = a * 2, y = a + 10 };
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n15\n");  // 5*2=10, 5+10=15
}

TEST_CASE("E2E - Struct literal with 64-bit field") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i64;
        }

        fun main(): i32 {
            var d = Data { a = 10, b = 100000000000l };
            print(d.a);
            print_i64(d.b);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n100000000000\n");
}

// ============================================================================
// Struct Parameter Tests
// ============================================================================

TEST_CASE("E2E - Small struct parameter") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun distance_sq(p: Point): i32 {
            return p.x * p.x + p.y * p.y;
        }

        fun main(): i32 {
            var pt = Point { x = 3, y = 4 };
            print(distance_sq(pt));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "25\n");  // 3^2 + 4^2 = 9 + 16
}

TEST_CASE("E2E - Struct parameter value semantics") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun modify(p: Point): i32 {
            p.x = 100;
            return p.x;
        }

        fun main(): i32 {
            var pt = Point { x = 5, y = 10 };
            print(modify(pt));
            print(pt.x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n5\n");  // modify returns 100, but pt.x is still 5 (value semantics)
}

TEST_CASE("E2E - Multiple struct parameters") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun add(a: Point, b: Point): i32 {
            print(a.x);
            print(a.y);
            print(b.x);
            print(b.y);
            return 0;
        }

        fun main(): i32 {
            var p1 = Point { x = 1, y = 2 };
            var p2 = Point { x = 10, y = 20 };
            add(p1, p2);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n10\n20\n");
}

TEST_CASE("E2E - Mixed struct and primitive parameters") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun scale(p: Point, factor: i32): i32 {
            print(p.x);
            print(p.y);
            print(factor);
            return (p.x + p.y) * factor;
        }

        fun main(): i32 {
            var pt = Point { x = 3, y = 7 };
            print(scale(pt, 5));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "3\n7\n5\n50\n");  // p.x, p.y, factor, result
}

// ============================================================================
// Struct Return Tests
// ============================================================================

TEST_CASE("E2E - Return small struct") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun make_point(x: i32, y: i32): Point {
            var p = Point { x = x, y = y };
            return p;
        }

        fun main(): i32 {
            var pt = make_point(10, 20);
            print(pt.x);
            print(pt.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Return struct with modification") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun double_point(p: Point): Point {
            var result = Point { x = p.x * 2, y = p.y * 2 };
            return result;
        }

        fun main(): i32 {
            var p1 = Point { x = 5, y = 10 };
            var p2 = double_point(p1);
            print(p2.x);
            print(p2.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");  // 5*2, 10*2
}

TEST_CASE("E2E - Chain struct returns") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun origin(): Point {
            return Point { x = 0, y = 0 };
        }

        fun offset(p: Point, dx: i32, dy: i32): Point {
            return Point { x = p.x + dx, y = p.y + dy };
        }

        fun main(): i32 {
            var p = offset(origin(), 5, 10);
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n10\n");
}

TEST_CASE("E2E - Struct return used in expression") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun make_point(x: i32, y: i32): Point {
            return Point { x = x, y = y };
        }

        fun main(): i32 {
            print(make_point(7, 8).x);
            print(make_point(3, 4).y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "7\n4\n");
}

// ============================================================================
// Large Struct Return Tests (>16 bytes / >4 slots)
// ============================================================================

TEST_CASE("E2E - Large struct return (>16 bytes)") {
    const char* source = R"(
        struct BigData {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
            e: i32;
        }

        fun make_big(x: i32): BigData {
            return BigData { a = x, b = x + 1, c = x + 2, d = x + 3, e = x + 4 };
        }

        fun main(): i32 {
            var data = make_big(10);
            print(data.a);
            print(data.b);
            print(data.c);
            print(data.d);
            print(data.e);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n11\n12\n13\n14\n");
}

TEST_CASE("E2E - Large struct return value semantics") {
    const char* source = R"(
        struct BigData { a: i32; b: i32; c: i32; d: i32; e: i32; }

        fun modify_big(data: BigData): i32 {
            data.a = 999;
            return data.a;
        }

        fun make_big(): BigData {
            return BigData { a = 1, b = 2, c = 3, d = 4, e = 5 };
        }

        fun main(): i32 {
            var data = make_big();
            print(modify_big(data));
            print(data.a);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "999\n1\n");  // modify_big returns 999, but data.a is still 1
}

TEST_CASE("E2E - Large struct chained returns") {
    const char* source = R"(
        struct BigData { a: i32; b: i32; c: i32; d: i32; e: i32; }

        fun make_big(x: i32): BigData {
            return BigData { a = x, b = x, c = x, d = x, e = x };
        }

        fun sum_big(data: BigData): i32 {
            return data.a + data.b + data.c + data.d + data.e;
        }

        fun main(): i32 {
            print(sum_big(make_big(5)));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "25\n");  // 5 * 5
}

TEST_CASE("E2E - Large struct return with 64-bit field") {
    const char* source = R"(
        struct BigData {
            a: i32;
            b: i32;
            c: i64;
            d: i32;
        }

        fun make_big(): BigData {
            return BigData { a = 1, b = 2, c = 100000000000l, d = 4 };
        }

        fun main(): i32 {
            var data = make_big();
            print(data.a);
            print(data.b);
            print_i64(data.c);
            print(data.d);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n100000000000\n4\n");
}

TEST_CASE("E2E - Large struct return used in expression") {
    const char* source = R"(
        struct BigData { a: i32; b: i32; c: i32; d: i32; e: i32; }

        fun make_big(x: i32): BigData {
            return BigData { a = x, b = x * 2, c = x * 3, d = x * 4, e = x * 5 };
        }

        fun main(): i32 {
            print(make_big(2).c);
            print(make_big(3).d);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "6\n12\n");  // 2*3=6, 3*4=12
}

TEST_CASE("E2E - Large struct return chained function calls") {
    const char* source = R"(
        struct BigData { a: i32; b: i32; c: i32; d: i32; e: i32; }

        fun make_big(x: i32): BigData {
            return BigData { a = x, b = x, c = x, d = x, e = x };
        }

        fun add_big(a: BigData, b: BigData): BigData {
            return BigData {
                a = a.a + b.a,
                b = a.b + b.b,
                c = a.c + b.c,
                d = a.d + b.d,
                e = a.e + b.e
            };
        }

        fun main(): i32 {
            var result = add_big(make_big(10), make_big(5));
            print(result.a);
            print(result.b);
            print(result.c);
            print(result.d);
            print(result.e);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "15\n15\n15\n15\n15\n");  // 10+5 for each field
}

// ============================================================================
// Float Struct Field Tests
// ============================================================================

TEST_CASE("E2E - Struct with f32 fields") {
    const char* source = R"(
        struct Point {
            x: f32;
            y: f32;
        }

        fun main(): i32 {
            var p: Point = Point { x = 1.5f, y = 2.5f };
            var sum: f32 = p.x + p.y;
            return i32(sum);
        }
    )";

    TestResult result = run_and_capture(source, "main", {}, true);
    CHECK(result.success);
    CHECK(result.value == 4);  // 1.5 + 2.5 = 4.0 -> 4
}

TEST_CASE("E2E - Struct with mixed i32 and f32 fields") {
    const char* source = R"(
        struct Data {
            count: i32;
            value: f32;
        }

        fun main(): i32 {
            var d: Data = Data { count = 10, value = 2.5f };
            var result: f32 = f32(d.count) * d.value;
            return i32(result);
        }
    )";

    TestResult result = run_and_capture(source, "main", {}, true);
    CHECK(result.success);
    CHECK(result.value == 25);  // 10 * 2.5 = 25.0 -> 25
}

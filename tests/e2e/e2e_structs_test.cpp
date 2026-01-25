#include "roxy/core/doctest/doctest.h"
#include "e2e_test_helpers.hpp"

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
            return p.x + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);  // 10 + 20
}

TEST_CASE("E2E - Struct with 64-bit field") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i64;
        }

        fun main(): i64 {
            var d: Data;
            d.a = 10;
            d.b = 100000000000;
            return d.a + d.b;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 100000000010);  // 10 + 100000000000
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
            return p1.x + p1.y + p2.x + p2.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 10);  // 1 + 2 + 3 + 4
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
            return p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 17);  // 12 + 5
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

    Value result = compile_and_run(source, StringView("main"));
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
                return p.x;
            } else {
                return p.y;
            }
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 10);
}

TEST_CASE("E2E - Struct in loop") {
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun main(): i32 {
            var c: Counter;
            c.value = 0;
            for (var i: i32 = 0; i < 10; i = i + 1) {
                c.value = c.value + i;
            }
            return c.value;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 45);  // 0 + 1 + 2 + ... + 9
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
            return r.origin.x + r.origin.y + r.size.x + r.size.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 330);  // 10 + 20 + 100 + 200
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
            return o.id + o.middle.data + o.middle.inner.value;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 111);  // 1 + 10 + 100
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

            return line1.end.x + line2.start.x;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 15);  // 10 + 5
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
            return p.x + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);
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
            return c.width + c.height + c.fullscreen;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 2520);  // 1920 + 600 + 0
}

TEST_CASE("E2E - Struct literal field order") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p = Point { y = 20, x = 10 };
            return p.x * 100 + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1020);  // 10 * 100 + 20
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
            return d.a + d.b + d.c;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 6);  // 1 + 2 + 3
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
            return p.x + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 25);  // (5 * 2) + (5 + 10) = 10 + 15
}

TEST_CASE("E2E - Struct literal with 64-bit field") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i64;
        }

        fun main(): i64 {
            var d = Data { a = 10, b = 100000000000 };
            return d.a + d.b;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 100000000010);  // 10 + 100000000000
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
            return distance_sq(pt);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 25);  // 3^2 + 4^2 = 9 + 16
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
            modify(pt);
            return pt.x;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 5);  // Should still be 5 (value semantics)
}

TEST_CASE("E2E - Multiple struct parameters") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun add(a: Point, b: Point): i32 {
            return a.x + b.x + a.y + b.y;
        }

        fun main(): i32 {
            var p1 = Point { x = 1, y = 2 };
            var p2 = Point { x = 10, y = 20 };
            return add(p1, p2);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 33);  // 1 + 10 + 2 + 20
}

TEST_CASE("E2E - Mixed struct and primitive parameters") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun scale(p: Point, factor: i32): i32 {
            return (p.x + p.y) * factor;
        }

        fun main(): i32 {
            var pt = Point { x = 3, y = 7 };
            return scale(pt, 5);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 50);  // (3 + 7) * 5
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
            return pt.x + pt.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);  // 10 + 20
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
            return p2.x + p2.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);  // (5*2) + (10*2) = 10 + 20
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
            return p.x + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 15);  // 5 + 10
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
            return make_point(7, 8).x + make_point(3, 4).y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 11);  // 7 + 4
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
            return data.a + data.b + data.c + data.d + data.e;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 60);  // 10 + 11 + 12 + 13 + 14
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
            modify_big(data);
            return data.a;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // Should still be 1 (value semantics)
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
            return sum_big(make_big(5));
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 25);  // 5 * 5
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
            return BigData { a = 1, b = 2, c = 100000000000, d = 4 };
        }

        fun main(): i64 {
            var data = make_big();
            return data.a + data.b + data.c + data.d;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 100000000007);  // 1 + 2 + 100000000000 + 4
}

TEST_CASE("E2E - Large struct return used in expression") {
    const char* source = R"(
        struct BigData { a: i32; b: i32; c: i32; d: i32; e: i32; }

        fun make_big(x: i32): BigData {
            return BigData { a = x, b = x * 2, c = x * 3, d = x * 4, e = x * 5 };
        }

        fun main(): i32 {
            return make_big(2).c + make_big(3).d;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 18);  // (2*3) + (3*4) = 6 + 12
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
            return result.a + result.e;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);  // (10+5) + (10+5) = 15 + 15
}

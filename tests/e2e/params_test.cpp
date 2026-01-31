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
            var pt = Point { x = 10, y = 20 };
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
            var pt = Point { x = 10, y = 20 };
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
            var rect = Rect {
                origin = Point { x = 1, y = 2 },
                size = Point { x = 10, y = 20 }
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

// ============================================================================
// Multi-Register Struct Parameter Tests
// These tests verify correct argument placement when struct parameters
// span multiple registers (3-4 slots = 2 registers)
// ============================================================================

TEST_CASE("E2E - Four-slot struct followed by int parameter") {
    // FourSlot is 4 slots (16 bytes) = 2 registers
    // The int parameter should be in register 2, not register 1
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun sum_with_extra(s: FourSlot, extra: i32): i32 {
            return s.a + s.b + s.c + s.d + extra;
        }

        fun main(): i32 {
            var s = FourSlot { a = 1, b = 2, c = 3, d = 4 };
            print(sum_with_extra(s, 100));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "110\n");  // 1+2+3+4+100 = 110
}

TEST_CASE("E2E - Three-slot struct followed by int parameter") {
    // ThreeSlot is 3 slots (12 bytes) = 2 registers
    // The int parameter should be in register 2, not register 1
    const char* source = R"(
        struct ThreeSlot {
            a: i32;
            b: i32;
            c: i32;
        }

        fun sum_with_extra(s: ThreeSlot, extra: i32): i32 {
            return s.a + s.b + s.c + extra;
        }

        fun main(): i32 {
            var s = ThreeSlot { a = 10, b = 20, c = 30 };
            print(sum_with_extra(s, 500));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "560\n");  // 10+20+30+500 = 560
}

TEST_CASE("E2E - Four-slot struct followed by two int parameters") {
    // FourSlot uses regs 0-1, ints should be in regs 2-3
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun compute(s: FourSlot, x: i32, y: i32): i32 {
            return s.a * x + s.d * y;
        }

        fun main(): i32 {
            var s = FourSlot { a = 5, b = 0, c = 0, d = 7 };
            print(compute(s, 10, 20));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "190\n");  // 5*10 + 7*20 = 50 + 140 = 190
}

TEST_CASE("E2E - Two four-slot struct parameters") {
    // First FourSlot in regs 0-1, second FourSlot in regs 2-3
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun add_structs(x: FourSlot, y: FourSlot): i32 {
            return x.a + x.b + x.c + x.d + y.a + y.b + y.c + y.d;
        }

        fun main(): i32 {
            var s1 = FourSlot { a = 1, b = 2, c = 3, d = 4 };
            var s2 = FourSlot { a = 10, b = 20, c = 30, d = 40 };
            print(add_structs(s1, s2));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "110\n");  // (1+2+3+4) + (10+20+30+40) = 10 + 100 = 110
}

TEST_CASE("E2E - Mix of two-slot and four-slot struct parameters") {
    // TwoSlot (2 slots = 1 reg), FourSlot (4 slots = 2 regs), int
    // TwoSlot in reg 0, FourSlot in regs 1-2, int in reg 3
    const char* source = R"(
        struct TwoSlot {
            x: i32;
            y: i32;
        }

        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun mixed(t: TwoSlot, f: FourSlot, extra: i32): i32 {
            return t.x + t.y + f.a + f.b + f.c + f.d + extra;
        }

        fun main(): i32 {
            var t = TwoSlot { x = 1, y = 2 };
            var f = FourSlot { a = 10, b = 20, c = 30, d = 40 };
            print(mixed(t, f, 1000));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1103\n");  // 1+2+10+20+30+40+1000 = 1103
}

TEST_CASE("E2E - Four-slot struct parameter with struct return") {
    // Tests that multi-register args work with struct returns
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        struct TwoSlot {
            x: i32;
            y: i32;
        }

        fun extract(s: FourSlot, which: i32): TwoSlot {
            if (which == 0) {
                return TwoSlot { x = s.a, y = s.b };
            } else {
                return TwoSlot { x = s.c, y = s.d };
            }
        }

        fun main(): i32 {
            var s = FourSlot { a = 1, b = 2, c = 3, d = 4 };
            var t1 = extract(s, 0);
            var t2 = extract(s, 1);
            print(t1.x);
            print(t1.y);
            print(t2.x);
            print(t2.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n");
}

TEST_CASE("E2E - Int parameter before four-slot struct") {
    // int in reg 0, FourSlot in regs 1-2
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun multiply_struct(factor: i32, s: FourSlot): i32 {
            return factor * (s.a + s.b + s.c + s.d);
        }

        fun main(): i32 {
            var s = FourSlot { a = 1, b = 2, c = 3, d = 4 };
            print(multiply_struct(10, s));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n");  // 10 * (1+2+3+4) = 10 * 10 = 100
}

TEST_CASE("E2E - Multiple four-slot structs with int in middle") {
    // FourSlot1 in regs 0-1, int in reg 2, FourSlot2 in regs 3-4
    const char* source = R"(
        struct FourSlot {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
        }

        fun weighted_sum(s1: FourSlot, weight: i32, s2: FourSlot): i32 {
            var sum1: i32 = s1.a + s1.b + s1.c + s1.d;
            var sum2: i32 = s2.a + s2.b + s2.c + s2.d;
            return sum1 * weight + sum2;
        }

        fun main(): i32 {
            var x = FourSlot { a = 1, b = 1, c = 1, d = 1 };
            var y = FourSlot { a = 5, b = 5, c = 5, d = 5 };
            print(weighted_sum(x, 10, y));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "60\n");  // (1+1+1+1)*10 + (5+5+5+5) = 40 + 20 = 60
}

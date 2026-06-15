#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

#include <string>

using namespace rx;

// ============================================================================
// Out/Inout Parameter Tests
// ============================================================================

TEST_SUITE("E2E Parameters") {

    TEST_CASE_TEMPLATE("Basic inout parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun increment(x: inout i32) {
            x = x + 1;
        }

        fun main(): i32 {
            var n: i32 = 41;
            increment(inout n);
            print(f"{n}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("Basic out parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun init_value(x: out i32) {
            x = 42;
        }

        fun main(): i32 {
            var n: i32 = 0;
            init_value(out n);
            print(f"{n}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("Multiple out parameters", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun init_pair(a: out i32, b: out i32) {
            a = 10;
            b = 20;
        }

        fun main(): i32 {
            var x: i32 = 0;
            var y: i32 = 0;
            init_pair(out x, out y);
            print(f"{x}");
            print(f"{y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n20\n");
    }

    TEST_CASE_TEMPLATE("Swap with inout", Backend, RX_E2E_BACKENDS) {
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
            print(f"{x}");
            print(f"{y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "20\n10\n");  // swapped
    }

    TEST_CASE_TEMPLATE("Inout with computation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun double_value(x: inout i32) {
            x = x * 2;
        }

        fun main(): i32 {
            var n: i32 = 21;
            double_value(inout n);
            print(f"{n}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");  // 21 * 2
    }

    TEST_CASE_TEMPLATE("Multiple inout calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun increment(x: inout i32) {
            x = x + 1;
        }

        fun main(): i32 {
            var n: i32 = 0;
            increment(inout n);
            print(f"{n}");
            increment(inout n);
            print(f"{n}");
            increment(inout n);
            print(f"{n}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n3\n");
    }

    TEST_CASE_TEMPLATE("Mixed regular and inout parameters", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun add_to(x: inout i32, amount: i32) {
            x = x + amount;
        }

        fun main(): i32 {
            var n: i32 = 10;
            add_to(inout n, 32);
            print(f"{n}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");  // 10 + 32
    }

    TEST_CASE_TEMPLATE("Inout with struct parameter", Backend, RX_E2E_BACKENDS) {
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
            print(f"{pt.x}");
            print(f"{pt.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "20\n40\n");  // 10*2, 20*2
    }

    TEST_CASE_TEMPLATE("Out with struct parameter", Backend, RX_E2E_BACKENDS) {
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
            print(f"{pt.x}");
            print(f"{pt.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "15\n27\n");
    }

    TEST_CASE_TEMPLATE("Inout struct field modification", Backend, RX_E2E_BACKENDS) {
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
            print(f"{pt.x}");
            print(f"{pt.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "20\n10\n");  // swapped
    }

    TEST_CASE_TEMPLATE("Inout with nested struct", Backend, RX_E2E_BACKENDS) {
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
            print(f"{rect.origin.x}");
            print(f"{rect.origin.y}");
            print(f"{rect.size.x}");
            print(f"{rect.size.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n6\n30\n60\n");  // 1*3, 2*3, 10*3, 20*3
    }

    // ============================================================================
    // Multi-Register Struct Parameter Tests
    // These tests verify correct argument placement when struct parameters
    // span multiple registers (3-4 slots = 2 registers)
    // ============================================================================

    TEST_CASE_TEMPLATE("Four-slot struct followed by int parameter", Backend, RX_E2E_BACKENDS) {
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
            print(f"{sum_with_extra(s, 100)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "110\n");  // 1+2+3+4+100 = 110
    }

    TEST_CASE_TEMPLATE("Three-slot struct followed by int parameter", Backend, RX_E2E_BACKENDS) {
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
            print(f"{sum_with_extra(s, 500)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "560\n");  // 10+20+30+500 = 560
    }

    TEST_CASE_TEMPLATE("Four-slot struct followed by two int parameters", Backend, RX_E2E_BACKENDS) {
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
            print(f"{compute(s, 10, 20)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "190\n");  // 5*10 + 7*20 = 50 + 140 = 190
    }

    TEST_CASE_TEMPLATE("Two four-slot struct parameters", Backend, RX_E2E_BACKENDS) {
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
            print(f"{add_structs(s1, s2)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "110\n");  // (1+2+3+4) + (10+20+30+40) = 10 + 100 = 110
    }

    TEST_CASE_TEMPLATE("Mix of two-slot and four-slot struct parameters", Backend, RX_E2E_BACKENDS) {
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
            print(f"{mixed(t, f, 1000)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1103\n");  // 1+2+10+20+30+40+1000 = 1103
    }

    TEST_CASE_TEMPLATE("Four-slot struct parameter with struct return", Backend, RX_E2E_BACKENDS) {
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
            print(f"{t1.x}");
            print(f"{t1.y}");
            print(f"{t2.x}");
            print(f"{t2.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n3\n4\n");
    }

    TEST_CASE_TEMPLATE("Int parameter before four-slot struct", Backend, RX_E2E_BACKENDS) {
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
            print(f"{multiply_struct(10, s)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");  // 10 * (1+2+3+4) = 10 * 10 = 100
    }

    TEST_CASE_TEMPLATE("Multiple four-slot structs with int in middle", Backend, RX_E2E_BACKENDS) {
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
            print(f"{weighted_sum(x, 10, y)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "60\n");  // (1+1+1+1)*10 + (5+5+5+5) = 40 + 20 = 60
    }

    // ============================================================================
    // inout noncopyable args in loop body (regression: semantic analyzer treated
    // inout identically to a by-value move and rejected the call with
    // "moved in loop body without reassignment"; the IR builder also marked the
    // local as moved after the call, tracked the inout param as owned, and
    // failed to phi-merge the caller's local at the loop header).
    // ============================================================================

    TEST_CASE_TEMPLATE("inout List<uniq T> in loop body compiles and runs", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Item { pub v: i32; }

        fun add_one(xs: inout List<uniq Item>) {
            xs.push(uniq Item());
        }

        fun main(): i32 {
            var xs: List<uniq Item> = List<uniq Item>();
            var i: i32 = 0;
            while (i < 3) {
                add_one(inout xs);
                i = i + 1;
            }
            var n: i32 = i32(xs.len());
            return n;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("inout List<i32> in for loop body with post-loop read", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        fun push_val(xs: inout List<i32>, v: i32) {
            xs.push(v);
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            for (var i: i32 = 0; i < 5; i = i + 1) {
                push_val(inout xs, i * 10);
            }
            var sum: i32 = 0;
            var j: i32 = 0;
            var n: i32 = i32(xs.len());
            while (j < n) {
                sum = sum + xs[j];
                j = j + 1;
            }
            return sum;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 0 + 10 + 20 + 30 + 40);
    }

    TEST_CASE_TEMPLATE("inout noncopyable cleanup across multiple calls is not double-freed", Backend, RX_E2E_BACKENDS) {
        // The caller still owns `xs` after an inout call — the callee must not
        // mark xs as moved, or else post-call nullify would suppress cleanup.
        // Conversely the inout param inside the callee must NOT be tracked as an
        // owned local (borrow, not ownership).
        const char* source = R"ROXY(
        struct Item { pub v: i32; }

        fun fill(xs: inout List<uniq Item>) {
            xs.push(uniq Item());
            xs.push(uniq Item());
        }

        fun main(): i32 {
            var xs: List<uniq Item> = List<uniq Item>();
            fill(inout xs);
            fill(inout xs);
            return i32(xs.len());
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4);
    }

    TEST_CASE("call exceeding the 255-register window fails to compile (no hang)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        // A call whose argument window exceeds 255 registers must produce a
        // clean register-overflow error during lowering, not spin forever in
        // the register-window pre-allocation loop.
        std::string params, args;
        for (int i = 0; i < 300; i++) {
            if (i) { params += ", "; args += ", "; }
            params += "a" + std::to_string(i) + ": i32";
            args += std::to_string(i);
        }
        std::string source = "fun big(" + params + "): i32 { return a0; }\n"
                             "fun main(): i32 { return big(" + args + "); }\n";

        auto result = VMBackend::run(source.c_str());
        CHECK_FALSE(result.success);
    }

}  // TEST_SUITE("E2E Parameters")

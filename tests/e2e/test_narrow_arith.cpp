#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Narrow-integer arithmetic via Java/C#-style promotion to i32.
//
// Narrow integer types (u8/u16/i8/i16) have no native arithmetic — every
// binary/unary op promotes them to i32 and yields an i32 result. Storing the
// i32 result back into a narrow lvalue needs an explicit cast, EXCEPT compound
// assignment (`x op= y`), which auto-narrows as `x = T(x op y)`. u32/u64 are
// deliberately NOT promoted (they need native unsigned arithmetic — step 2).
//
// Values are checked via print/stdout rather than the return value: the C
// backend reports main()'s result through the process exit code, which the OS
// truncates to 0..255 (see test_e2e_backend.hpp), so returning e.g. 300 would
// spuriously read back as 44.
// ============================================================================

TEST_SUITE("E2E Narrow Arithmetic") {

    TEST_CASE_TEMPLATE("Arithmetic promotes narrow operands to i32", Backend, RX_E2E_BACKENDS) {
        SUBCASE("u8 + u8 does not wrap (result is i32)") {
            const char* source = R"(
            fun main(): i32 {
                var a: u8 = 200;
                var b: u8 = 100;
                print(f"{a + b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "300\n");  // would wrap to 44 at u8 width
        }

        SUBCASE("u16 * u16 does not wrap") {
            const char* source = R"(
            fun main(): i32 {
                var a: u16 = 300;
                var b: u16 = 300;
                print(f"{a * b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "90000\n");  // exceeds u16 max (65535)
        }

        SUBCASE("i8 subtraction yields signed i32") {
            const char* source = R"(
            fun main(): i32 {
                var a: i8 = 10;
                var b: i8 = 20;
                print(f"{a - b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "-10\n");
        }

        SUBCASE("u8 division and modulo") {
            const char* source = R"(
            fun main(): i32 {
                var a: u8 = 200;
                var b: u8 = 7;
                print(f"{a / b}");
                print(f"{a % b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "28\n4\n");
        }
    }

    TEST_CASE_TEMPLATE("Ordered comparison on narrow types", Backend, RX_E2E_BACKENDS) {
        SUBCASE("unsigned narrow compares correctly") {
            const char* source = R"(
            fun main(): i32 {
                var a: u8 = 200;
                var b: u8 = 100;
                var r: i32 = 0;
                if (a > b) { r = r + 1; }
                if (a < b) { r = r + 100; }
                if (a >= a) { r = r + 10; }
                if (b <= a) { r = r + 1000; }
                print(f"{r}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "1011\n");
        }

        SUBCASE("negative i8 compares as signed") {
            const char* source = R"(
            fun main(): i32 {
                var a: i8 = i8(-5);
                var b: i8 = i8(3);
                var r: i32 = 0;
                if (a < b) { r = r + 1; }
                if (a < 0) { r = r + 10; }
                print(f"{r}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "11\n");
        }
    }

    TEST_CASE_TEMPLATE("Bitwise and unary on narrow types", Backend, RX_E2E_BACKENDS) {
        SUBCASE("bitwise or/and/xor") {
            const char* source = R"(
            fun main(): i32 {
                var a: u8 = 0xF0;
                var b: u8 = 0x0F;
                print(f"{a | b}");
                print(f"{a & b}");
                print(f"{a ^ b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "255\n0\n255\n");
        }

        SUBCASE("shifts promote to i32") {
            const char* source = R"(
            fun main(): i32 {
                var a: u8 = 3;
                var b: i8 = i8(-16);
                print(f"{a << 4}");
                print(f"{b >> 2}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "48\n-4\n");  // 3<<4=48; arithmetic -16>>2=-4
        }

        SUBCASE("unary negate and bitnot") {
            const char* source = R"(
            fun main(): i32 {
                var a: i8 = i8(-5);
                var b: u8 = 0;
                print(f"{-a}");
                print(f"{~b}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "5\n-1\n");  // -(-5)=5; ~0 (i32) = -1
        }
    }

    TEST_CASE_TEMPLATE("Compound assignment auto-narrows", Backend, RX_E2E_BACKENDS) {
        SUBCASE("u8 += wraps back into u8") {
            const char* source = R"(
            fun main(): i32 {
                var x: u8 = 250;
                x += 10;
                print(f"{i32(x)}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "4\n");  // 260 & 0xFF = 4
        }

        SUBCASE("u8 -= underflows into u8") {
            const char* source = R"(
            fun main(): i32 {
                var x: u8 = 5;
                x -= 10;
                print(f"{i32(x)}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "251\n");  // -5 & 0xFF = 251
        }

        SUBCASE("u8 *= narrows the product") {
            const char* source = R"(
            fun main(): i32 {
                var x: u8 = 100;
                x *= 3;
                print(f"{i32(x)}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "44\n");  // 300 & 0xFF = 44
        }
    }

    TEST_CASE_TEMPLATE("Narrow value in a struct field", Backend, RX_E2E_BACKENDS) {
        // Exercises the GET_FIELD sign-extension path that promoted signed-narrow
        // arithmetic relies on: an i8 field read must arrive sign-extended to 64 bits.
        const char* source = R"(
        struct S { v: i8; }
        fun main(): i32 {
            var s: S = S { v = i8(-5) };
            var r: i32 = 0;
            if (s.v < 0) { r = r + 1; }
            if (s.v + s.v == -10) { r = r + 10; }
            print(f"{r}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "11\n");
    }

    TEST_CASE_TEMPLATE("Casting an unsuffixed literal to a narrow type", Backend, RX_E2E_BACKENDS) {
        SUBCASE("in-range u8(200)") {
            const char* source = R"(
            fun main(): i32 {
                var x: u8 = u8(200);
                print(f"{i32(x)}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "200\n");
        }

        SUBCASE("out-of-range u8(300) truncates") {
            const char* source = R"(
            fun main(): i32 {
                var x: u8 = u8(300);
                print(f"{i32(x)}");
                return 0;
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "44\n");  // 300 & 0xFF = 44
        }
    }

    TEST_CASE_TEMPLATE("Mixing a narrow operand with i32 promotes both", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: u8 = 200;
            var b: i32 = 100;
            print(f"{a + b}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "300\n");
    }

    TEST_CASE_TEMPLATE("Storing a narrow arithmetic result requires an explicit cast", Backend, RX_E2E_BACKENDS) {
        // Plain assignment does NOT auto-narrow: `a + b` is i32 and cannot be
        // stored into a u8 lvalue implicitly (compound assignment is the exception).
        const char* source = R"(
        fun main(): i32 {
            var a: u8 = 200;
            var b: u8 = 100;
            var c: u8 = a + b;
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("u32 arithmetic stays rejected", Backend, RX_E2E_BACKENDS) {
        // u32 is not promoted and does not yet have native arithmetic (it needs the
        // VM-side canonical-zero-extension + GET_FIELD fix); it must still fail to
        // compile. (u64 arithmetic now works — see test_unsigned_arith.cpp.)
        const char* source = R"(
        fun main(): i32 {
            var a: u32 = 40;
            var b: u32 = 2;
            var c = a + b;
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

}  // TEST_SUITE("E2E Narrow Arithmetic")

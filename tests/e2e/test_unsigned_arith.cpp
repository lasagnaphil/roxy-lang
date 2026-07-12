#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Native u64 arithmetic (wrapping unsigned). u64 is full-width, so add/sub/mul/
// shl/bitwise reuse the shared 64-bit ops (wrap natively); division, modulo,
// ordered comparison, and logical shift-right use the unsigned IR ops
// (DivU/ModU/LtU../UShr → DIV_U/MOD_U/LT_U../USHR). Operands are variables so the
// runtime opcodes actually execute (constant-folded forms are covered too since
// fold_binary_const handles the unsigned ops).
//
// Values checked via print/stdout (the C backend reports main()'s result as an
// 8-bit exit code). Reference constants: 2^63 = 9223372036854775808 (0x8000…),
// 2^62 = 4611686018427387904 (0x4000…), 2^64-1 = 18446744073709551615.
// ============================================================================

TEST_SUITE("E2E Unsigned Arithmetic") {

    TEST_CASE_TEMPLATE("Ordered comparison is unsigned", Backend, RX_E2E_BACKENDS) {
        // 2^63 has the sign bit set: as u64 it is huge (> 1); as i64 it is negative.
        const char* source = R"(
        fun main(): i32 {
            var a: u64 = 0x8000000000000000ul;
            var b: u64 = 1ul;
            print(f"{a > b}");
            print(f"{a < b}");
            print(f"{a >= a}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\nfalse\ntrue\n");
    }

    TEST_CASE_TEMPLATE("Unsigned division and modulo", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: u64 = 0x8000000000000000ul;
            var two: u64 = 2ul;
            var three: u64 = 3ul;
            print(f"{a / two}");
            print(f"{a % three}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        // 2^63 / 2 = 2^62 (signed would negate); 2^63 % 3 = 2.
        CHECK(result.stdout_output == "4611686018427387904\n2\n");
    }

    TEST_CASE_TEMPLATE("Logical (unsigned) shift right", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: u64 = 0x8000000000000000ul;
            var one: u64 = 1ul;
            print(f"{a >> one}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        // Logical: 0x8000…>>1 = 0x4000… = 2^62. Arithmetic would keep the top bit.
        CHECK(result.stdout_output == "4611686018427387904\n");
    }

    TEST_CASE_TEMPLATE("Add/mul wrap at 64 bits", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: u64 = 0xFFFFFFFFFFFFFFFFul;
            var two: u64 = 2ul;
            print(f"{a + two}");
            print(f"{a * two}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        // (2^64-1)+2 = 1 (mod 2^64); (2^64-1)*2 = 2^64-2 (mod 2^64).
        CHECK(result.stdout_output == "1\n18446744073709551614\n");
    }

    TEST_CASE_TEMPLATE("Bitwise ops on u64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: u64 = 0xF0ul;
            var b: u64 = 0x0Ful;
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

    TEST_CASE_TEMPLATE("Compound assignment on u64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: u64 = 0x8000000000000000ul;
            x >>= 1ul;
            print(f"{x}");
            var h: u64 = 0xFFFFFFFFFFFFFFFFul;
            h *= 2ul;
            print(f"{h}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "4611686018427387904\n18446744073709551614\n");
    }

    TEST_CASE_TEMPLATE("Wrapping FNV-1a-style hash", Backend, RX_E2E_BACKENDS) {
        // A real wrapping-multiply loop: mix four bytes into a 64-bit hash.
        const char* source = R"(
        fun main(): i32 {
            var h: u64 = 14695981039346656037ul;
            var prime: u64 = 1099511628211ul;
            var i: u64 = 1ul;
            while (i <= 4ul) {
                h = h ^ i;
                h = h * prime;
                i = i + 1ul;
            }
            print(f"{h}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        // Deterministic wrapping result; both backends must agree.
        CHECK(result.stdout_output == "13725386680924731485\n");
    }

    TEST_CASE_TEMPLATE("u64 prints as unsigned", Backend, RX_E2E_BACKENDS) {
        // A value above 2^63 must print as a large positive number, not negative.
        const char* source = R"(
        fun main(): i32 {
            var x: u64 = 12345678901234567890ul;
            print(f"{x}");
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "12345678901234567890\n");
    }

}  // TEST_SUITE("E2E Unsigned Arithmetic")

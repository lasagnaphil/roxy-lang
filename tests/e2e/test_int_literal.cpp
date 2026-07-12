#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Integer Literal Polymorphism Tests
// ============================================================================

TEST_SUITE("E2E Int Literals") {

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to i64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to i8", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun check(x: i8): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: i8 = 42;
            print(f"{check(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to u32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun check(x: u32): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: u32 = 42;
            print(f"{check(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to u64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: u64 = 42;
            print(f"{i64(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral default inference is i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral function argument coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun add_one(x: i64): i64 {
            return x + 1l;
        }

        fun main(): i32 {
            print(f"{add_one(42)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral return statement coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun get_value(): i64 {
            return 42;
        }

        fun main(): i32 {
            print(f"{get_value()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral binary ops with suffixed literal", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: i64 = 42 + 1l;
            print(f"{a}");
            var b: i64 = 1l + 42;
            print(f"{b}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral binary ops default to i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x = 42 + 1;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral unary negate coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = -42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "-42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral compound assignment", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x += 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral ternary coercion compiles", Backend, RX_E2E_BACKENDS) {
        // Ternary with int literal coercion should compile (runtime value depends
        // on simplified ternary implementation that always takes the else branch)
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = false ? 0l : 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral non-literal still strict", Backend, RX_E2E_BACKENDS) {
        // A concrete i32 variable should NOT be assignable to i64
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var y: i64 = x;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("IntLiteral simple assignment coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral struct field init", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i64;
            y: i64;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            print(f"{p.x + p.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral comparison with typed variable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42l;
            if (x == 42) {
                print(f"{1}");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral non-integer target fails", Backend, RX_E2E_BACKENDS) {
        // Cannot assign integer literal to string
        const char* source = R"(
        fun main(): i32 {
            var x: bool = 42;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("IntLiteral suffixed literal still strict", Backend, RX_E2E_BACKENDS) {
        // 42l is i64, cannot assign to i32
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42l;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Equality works on all integer kinds", Backend, RX_E2E_BACKENDS) {
        // eq/ne used to be registered only for i32/i64; u8..u64 comparisons
        // silently typed as Error. Equality is a bit comparison on canonical
        // values, so it is now registered for every integer kind.
        const char* source = R"(
        fun main(): i32 {
            var a: u32 = 40;
            var b: u32 = 40;
            var c: u64 = 9000000000ul;
            var d: u64 = 9000000000ul;
            var r: i32 = 0;
            if (a == b) { r = r + 1; }
            if (c == d) { r = r + 2; }
            if (a != b) { r = r + 100; }
            return r;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Unsigned arithmetic is supported", Backend, RX_E2E_BACKENDS) {
        // u32 + u32 once typed as Error with no diagnostic; it now has native
        // unsigned arithmetic (see test_unsigned_arith.cpp for the semantics).
        const char* source = R"(
        fun main(): i32 {
            var x: u32 = 40;
            var y: u32 = 2;
            var z: u32 = x + y;
            print(f"{z}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

}  // TEST_SUITE("E2E Int Literals")

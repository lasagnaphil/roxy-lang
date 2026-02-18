#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Integer Literal Polymorphism Tests
// ============================================================================

TEST_CASE("E2E - IntLiteral basic coercion to i64") {
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42;
            print_i64(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral basic coercion to i8") {
    const char* source = R"(
        fun check(x: i8): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: i8 = 42;
            print(check(x));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral basic coercion to u32") {
    const char* source = R"(
        fun check(x: u32): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: u32 = 42;
            print(check(x));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral basic coercion to u64") {
    const char* source = R"(
        fun main(): i32 {
            var x: u64 = 42;
            print_i64(i64(x));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral default inference is i32") {
    const char* source = R"(
        fun main(): i32 {
            var x = 42;
            print(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral function argument coercion") {
    const char* source = R"(
        fun add_one(x: i64): i64 {
            return x + 1l;
        }

        fun main(): i32 {
            print_i64(add_one(42));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "43\n");
}

TEST_CASE("E2E - IntLiteral return statement coercion") {
    const char* source = R"(
        fun get_value(): i64 {
            return 42;
        }

        fun main(): i32 {
            print_i64(get_value());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral binary ops with suffixed literal") {
    const char* source = R"(
        fun main(): i32 {
            var a: i64 = 42 + 1l;
            print_i64(a);
            var b: i64 = 1l + 42;
            print_i64(b);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "43\n43\n");
}

TEST_CASE("E2E - IntLiteral binary ops default to i32") {
    const char* source = R"(
        fun main(): i32 {
            var x = 42 + 1;
            print(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "43\n");
}

TEST_CASE("E2E - IntLiteral unary negate coercion") {
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = -42;
            print_i64(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "-42\n");
}

TEST_CASE("E2E - IntLiteral compound assignment") {
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x += 42;
            print_i64(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral ternary coercion compiles") {
    // Ternary with int literal coercion should compile (runtime value depends
    // on simplified ternary implementation that always takes the else branch)
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = false ? 0l : 42;
            print_i64(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral non-literal still strict") {
    // A concrete i32 variable should NOT be assignable to i64
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var y: i64 = x;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - IntLiteral simple assignment coercion") {
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x = 42;
            print_i64(x);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - IntLiteral struct field init") {
    const char* source = R"(
        struct Point {
            x: i64;
            y: i64;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            print_i64(p.x + p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");
}

TEST_CASE("E2E - IntLiteral comparison with typed variable") {
    const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42l;
            if (x == 42) {
                print(1);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n");
}

TEST_CASE("E2E - IntLiteral non-integer target fails") {
    // Cannot assign integer literal to string
    const char* source = R"(
        fun main(): i32 {
            var x: bool = 42;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - IntLiteral suffixed literal still strict") {
    // 42l is i64, cannot assign to i32
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42l;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

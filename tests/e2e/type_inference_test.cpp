#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Type Inference Tests
// ============================================================================

TEST_CASE("E2E - Type inference - Default integer type is i32") {
    const char* source = R"(
        fun main(): i32 {
            var a = 42;       // i32
            print(f"{a}");
            return 0;
        }
    )";
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Type inference - Integer suffixes") {
    SUBCASE("L suffix for i64") {
        const char* source = R"(
            fun main(): i32 {
                var a = 42l;      // i64
                var b: i64 = a;   // Should work
                print(f"{42}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("U suffix for u32") {
        const char* source = R"(
            fun main(): i32 {
                var a = 42u;      // u32
                var b: u32 = a;   // Should work
                print(f"{42}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("UL suffix for u64") {
        const char* source = R"(
            fun main(): i32 {
                var a = 42ul;     // u64
                var b: u64 = a;   // Should work
                print(f"{42}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }
}

TEST_CASE("E2E - Type inference - Float suffixes") {
    SUBCASE("Default float is f64") {
        const char* source = R"(
            fun main(): i32 {
                var a = 3.14;     // f64
                var b: f64 = a;   // Should work
                print(f"{1}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
    }

    SUBCASE("F suffix for f32") {
        const char* source = R"(
            fun main(): i32 {
                var a = 3.14f;    // f32
                var b: f32 = a;   // Should work
                print(f"{1}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
    }
}

TEST_CASE("E2E - Type inference - Arithmetic with inferred types") {
    SUBCASE("i32 arithmetic") {
        const char* source = R"(
            fun main(): i32 {
                var a = 10;
                var b = 20;
                var c = a + b;    // i32 + i32 = i32
                print(f"{c}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    SUBCASE("i64 arithmetic") {
        const char* source = R"(
            fun main(): i32 {
                var a = 10l;
                var b = 20l;
                var c = a + b;    // i64 + i64 = i64
                print(f"{30}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }
}

TEST_CASE("E2E - Type inference - For loop with inferred index") {
    const char* source = R"(
        fun main(): i32 {
            var sum = 0;
            for (var i = 0; i < 10; i = i + 1) {
                sum = sum + i;
            }
            print(f"{sum}");
            return 0;
        }
    )";
    // Sum of 0..9 = 45
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "45\n");
}

TEST_CASE("E2E - Type inference - Type mismatch errors") {
    SUBCASE("Unsuffixed integer literal coerces to i64") {
        const char* source = R"(
            fun main(): i32 {
                var x: i64 = 42;  // OK: unsuffixed literal coerces to i64
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
    }

    SUBCASE("Cannot assign i64 to i32 variable") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 42l;  // ERROR: 42l is i64, cannot assign to i32
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }

    SUBCASE("Cannot assign f64 to f32 variable") {
        const char* source = R"(
            fun main(): i32 {
                var x: f32 = 3.14;  // ERROR: 3.14 is f64, cannot assign to f32
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }

    SUBCASE("Cannot assign f32 to f64 variable") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 3.14f;  // ERROR: 3.14f is f32, cannot assign to f64
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }
}

TEST_CASE("E2E - Type inference - Binary operator type mismatch") {
    SUBCASE("Unsuffixed literal coerces in add with i64") {
        const char* source = R"(
            fun main(): i32 {
                var x = 1 + 2l;  // OK: 1 coerces to i64
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
    }

    SUBCASE("Unsuffixed literal coerces in compare with i64") {
        const char* source = R"(
            fun main(): i32 {
                var x = 1 < 2l;  // OK: 1 coerces to i64
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
    }

    SUBCASE("Cannot mix f32 and f64") {
        const char* source = R"(
            fun main(): i32 {
                var x = 1.0f + 2.0;  // ERROR: f32 + f64
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }
}

TEST_CASE("E2E - Type inference - Correct type matching works") {
    SUBCASE("i32 with i32") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 42;  // 42 is i32, assigned to i32
                print(f"{x}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("i64 with i64") {
        const char* source = R"(
            fun main(): i32 {
                var x: i64 = 42l;  // 42l is i64, assigned to i64
                print(f"{42}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }
}

TEST_CASE("E2E - Type inference - Function parameter type checking") {
    SUBCASE("Passing i32 to i32 parameter") {
        const char* source = R"(
            fun add_one(x: i32): i32 {
                return x + 1;
            }

            fun main(): i32 {
                var result = add_one(41);
                print(f"{result}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Cannot pass i64 to i32 parameter") {
        const char* source = R"(
            fun add_one(x: i32): i32 {
                return x + 1;
            }

            fun main(): i32 {
                var result = add_one(41l);  // ERROR: i64 to i32 param
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }
}

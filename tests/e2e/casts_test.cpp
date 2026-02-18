#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Primitive Type Casting Tests
// ============================================================================

TEST_CASE("E2E - Cast integer truncation") {
    SUBCASE("i64 to i32") {
        const char* source = R"(
            fun main(): i32 {
                var x: i64 = 1000l;
                var y: i32 = i32(x);
                print(f"{y}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "1000\n");
    }

    SUBCASE("i32 to i16 (with truncation)") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 70000;
                var y: i16 = i16(x);
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // 70000 = 0x11170, truncated to 16 bits = 0x1170 = 4464
        CHECK(result.stdout_output == "4464\n");
    }

    SUBCASE("i32 to i8 (with truncation)") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 300;
                var y: i8 = i8(x);
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // 300 = 0x12C, truncated to 8 bits = 0x2C = 44
        CHECK(result.stdout_output == "44\n");
    }

    SUBCASE("Negative value truncation with sign extension") {
        const char* source = R"(
            fun main(): i32 {
                var x: i64 = -100l;
                var y: i32 = i32(x);
                print(f"{y}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "-100\n");
    }
}

TEST_CASE("E2E - Cast integer widening") {
    SUBCASE("i32 to i64") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 12345;
                var y: i64 = i64(x);
                print(f"{y}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "12345\n");
    }

    SUBCASE("i8 to i32 (sign extension)") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = -50;
                var y: i8 = i8(x);
                var z: i32 = i32(y);
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "-50\n");
    }

    SUBCASE("u8 to i32 (zero extension)") {
        const char* source = R"(
            fun main(): i32 {
                var x: u32 = 200u;
                var y: u8 = u8(x);
                var z: i32 = i32(y);
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "200\n");
    }
}

TEST_CASE("E2E - Cast float conversions") {
    SUBCASE("f32 to f64 to i32") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 3.14;
                var y: f32 = f32(x);
                var z: f64 = f64(y);
                var w: i32 = i32(z * 100.0);
                print(f"{w}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // f32 precision means 3.14 -> 3.14000010... -> 314
        CHECK(result.stdout_output == "314\n");
    }

    SUBCASE("f64 to f32 precision") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 2.718;
                var y: f32 = f32(x);
                var z: i32 = i32(f64(y) * 1000.0);
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // f32 has less precision: 2.718 -> 2.71799993... -> 2717
        CHECK(result.stdout_output == "2717\n");
    }
}

TEST_CASE("E2E - Cast integer to float") {
    SUBCASE("i32 to f64") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 42;
                var y: f64 = f64(x);
                var z: i32 = i32(y + 0.5);
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("i64 to f32") {
        const char* source = R"(
            fun main(): i32 {
                var x: i64 = 100l;
                var y: f32 = f32(x);
                var z: i32 = i32(f64(y));
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }
}

TEST_CASE("E2E - Cast float to integer") {
    SUBCASE("f64 to i32 (truncation toward zero)") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 3.7;
                var y: i32 = i32(x);
                print(f"{y}");
                var a: f64 = -3.7;
                var b: i32 = i32(a);
                print(f"{b}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n-3\n");
    }

    SUBCASE("f32 to i64") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 99.9;
                var y: f32 = f32(x);
                var z: i64 = i64(y);
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "99\n");
    }
}

TEST_CASE("E2E - Cast bool conversions") {
    SUBCASE("Integer to bool") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 42;
                var y: i32 = 0;
                var a: bool = bool(x);
                var b: bool = bool(y);
                if (a) { print(f"{1}"); } else { print(f"{0}"); }
                if (b) { print(f"{1}"); } else { print(f"{0}"); }
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n0\n");
    }

    SUBCASE("Bool to integer") {
        const char* source = R"(
            fun main(): i32 {
                var t: bool = true;
                var f: bool = false;
                var x: i32 = i32(t);
                var y: i32 = i32(f);
                print(f"{x}");
                print(f"{y}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n0\n");
    }

    SUBCASE("Float to bool") {
        const char* source = R"(
            fun main(): i32 {
                var x: f64 = 0.0;
                var y: f64 = 1.5;
                if (bool(x)) { print(f"{1}"); } else { print(f"{0}"); }
                if (bool(y)) { print(f"{1}"); } else { print(f"{0}"); }
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n1\n");
    }

    SUBCASE("Bool to float") {
        const char* source = R"(
            fun main(): i32 {
                var t: bool = true;
                var f: bool = false;
                var x: f64 = f64(t);
                var y: f64 = f64(f);
                print(f"{i32(x)}");
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n0\n");
    }
}

TEST_CASE("E2E - Cast no-op (same type)") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            var y: i32 = i32(x);
            print(f"{y}");
            return 0;
        }
    )";
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Cast in expressions") {
    SUBCASE("Mixed type arithmetic with casts") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 10;
                var y: i64 = 20l;
                var z: i64 = i64(x) + y;
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    SUBCASE("Chained casts") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 100;
                var y: f64 = f64(i64(x));
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }

    SUBCASE("Cast result used in function call") {
        const char* source = R"(
            fun add_longs(a: i64, b: i64): i64 {
                return a + b;
            }
            fun main(): i32 {
                var x: i32 = 5;
                var y: i32 = 7;
                var z: i64 = add_longs(i64(x), i64(y));
                print(f"{z}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "12\n");
    }
}

TEST_CASE("E2E - Cast unsigned types") {
    SUBCASE("u32 to i32") {
        const char* source = R"(
            fun main(): i32 {
                var x: u32 = 100u;
                var y: i32 = i32(x);
                print(f"{y}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }

    SUBCASE("i32 to u32") {
        const char* source = R"(
            fun main(): i32 {
                var x: i32 = 50;
                var y: u32 = u32(x);
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n");
    }

    SUBCASE("u64 to u32 truncation") {
        const char* source = R"(
            fun main(): i32 {
                var x: u64 = 4294967296ul;
                var y: u32 = u32(x);
                print(f"{i32(y)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // 4294967296 = 0x100000000, truncated to 32 bits = 0
        CHECK(result.stdout_output == "0\n");
    }
}

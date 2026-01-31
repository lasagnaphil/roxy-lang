#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Basic Tests
// ============================================================================

TEST_CASE("E2E - Return constant") {
    const char* source = R"(
        fun main(): i32 {
            return 42;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Arithmetic expressions") {
    SUBCASE("Addition") {
        const char* source = R"(
            fun main(): i32 {
                print(10 + 32);
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Complex expression") {
        const char* source = R"(
            fun main(): i32 {
                print(1 + 2 * 3 + 4 * 5);
                return 0;
            }
        )";
        // 1 + 6 + 20 = 27
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "27\n");
    }

    SUBCASE("Parentheses") {
        const char* source = R"(
            fun main(): i32 {
                print((1 + 2) * (3 + 4));
                return 0;
            }
        )";
        // 3 * 7 = 21
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("Division and modulo") {
        const char* source = R"(
            fun main(): i32 {
                print(100 / 3);
                print(100 % 3);
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "33\n1\n");
    }

    SUBCASE("Negation") {
        const char* source = R"(
            fun main(): i32 {
                print(-42);
                print(-(-42));
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "-42\n42\n");
    }
}

TEST_CASE("E2E - Local variables") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 10;
            var b: i32 = 20;
            var c: i32 = a + b;
            print(a);
            print(b);
            print(c);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n30\n");
}

TEST_CASE("E2E - Function calls") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }

        fun main(): i32 {
            print(add(17, 25));
            print(add(100, 200));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n300\n");
}

// ============================================================================
// Control Flow Tests
// ============================================================================

TEST_CASE("E2E - If statement") {
    const char* source = R"(
        fun abs(x: i32): i32 {
            if (x < 0) {
                return -x;
            }
            return x;
        }

        fun main(): i32 {
            print(abs(42));
            print(abs(-42));
            print(abs(0));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n42\n0\n");
}

TEST_CASE("E2E - If-else statement") {
    const char* source = R"(
        fun max(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            }
        }

        fun main(): i32 {
            print(max(10, 5));
            print(max(3, 7));
            print(max(5, 5));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n7\n5\n");
}

TEST_CASE("E2E - While loop") {
    const char* source = R"(
        fun sum_to_n(n: i32): i32 {
            var sum: i32 = 0;
            var i: i32 = 1;
            while (i <= n) {
                sum = sum + i;
                i = i + 1;
            }
            return sum;
        }

        fun main(): i32 {
            print(sum_to_n(10));
            print(sum_to_n(100));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "55\n5050\n");
}

TEST_CASE("E2E - For loop") {
    const char* source = R"(
        fun main(): i32 {
            var sum: i32 = 0;
            for (var i: i32 = 1; i <= 10; i = i + 1) {
                sum = sum + i;
                print(i);
            }
            print(sum);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n55\n");
}

TEST_CASE("E2E - Nested loops") {
    const char* source = R"(
        fun main(): i32 {
            for (var i: i32 = 1; i <= 3; i = i + 1) {
                for (var j: i32 = 1; j <= 3; j = j + 1) {
                    print(i * 10 + j);
                }
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "11\n12\n13\n21\n22\n23\n31\n32\n33\n");
}

TEST_CASE("E2E - Boolean expressions") {
    // Test AND operator with comparison expressions
    // NOTE: OR operator has a known bug with short-circuit evaluation
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 5;
            var b: i32 = 3;

            // AND with comparisons - all work correctly
            if ((a > b) && (b > 0)) { print(1); } else { print(0); }
            if ((a > b) && (b < 0)) { print(1); } else { print(0); }
            if ((a < b) && (b > 0)) { print(1); } else { print(0); }
            if ((a < b) && (b < 0)) { print(1); } else { print(0); }

            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // AND: (5>3)&&(3>0)=1, (5>3)&&(3<0)=0, (5<3)&&(3>0)=0, (5<3)&&(3<0)=0
    CHECK(result.stdout_output == "1\n0\n0\n0\n");
}

TEST_CASE("E2E - Comparison operators") {
    const char* source = R"(
        fun bool_to_int(b: bool): i32 {
            if (b) { return 1; }
            return 0;
        }

        fun main(): i32 {
            print(bool_to_int(5 == 5));
            print(bool_to_int(5 == 3));
            print(bool_to_int(5 != 3));
            print(bool_to_int(5 != 5));
            print(bool_to_int(3 < 5));
            print(bool_to_int(5 < 3));
            print(bool_to_int(5 <= 5));
            print(bool_to_int(3 > 5));
            print(bool_to_int(5 > 3));
            print(bool_to_int(5 >= 5));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // ==: 1,0; !=: 1,0; <: 1,0; <=: 1; >: 0,1; >=: 1
    CHECK(result.stdout_output == "1\n0\n1\n0\n1\n0\n1\n0\n1\n1\n");
}

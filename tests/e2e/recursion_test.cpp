#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Recursive Function Tests
// ============================================================================

TEST_CASE("E2E - Factorial (recursive)") {
    const char* source = R"(
        fun factorial(n: i32): i32 {
            if (n <= 1) {
                return 1;
            }
            return n * factorial(n - 1);
        }

        fun main(): i32 {
            print(f"{factorial(0)}");
            print(f"{factorial(1)}");
            print(f"{factorial(5)}");
            print(f"{factorial(10)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n1\n120\n3628800\n");
}

TEST_CASE("E2E - Fibonacci (recursive)") {
    const char* source = R"(
        fun fib(n: i32): i32 {
            if (n <= 1) {
                return n;
            }
            return fib(n - 1) + fib(n - 2);
        }

        fun main(): i32 {
            print(f"{fib(0)}");
            print(f"{fib(1)}");
            print(f"{fib(10)}");
            print(f"{fib(15)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n1\n55\n610\n");
}

TEST_CASE("E2E - GCD (Euclidean algorithm)") {
    const char* source = R"(
        fun gcd(a: i32, b: i32): i32 {
            if (b == 0) {
                return a;
            }
            return gcd(b, a % b);
        }

        fun main(): i32 {
            print(f"{gcd(48, 18)}");
            print(f"{gcd(100, 35)}");
            print(f"{gcd(17, 13)}");
            print(f"{gcd(12, 12)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // gcd(48,18)=6, gcd(100,35)=5, gcd(17,13)=1, gcd(12,12)=12
    CHECK(result.stdout_output == "6\n5\n1\n12\n");
}

TEST_CASE("E2E - Power function (recursive)") {
    const char* source = R"(
        fun power(base: i32, exp: i32): i32 {
            if (exp == 0) {
                return 1;
            }
            return base * power(base, exp - 1);
        }

        fun main(): i32 {
            print(f"{power(2, 0)}");
            print(f"{power(2, 10)}");
            print(f"{power(3, 5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // 2^0=1, 2^10=1024, 3^5=243
    CHECK(result.stdout_output == "1\n1024\n243\n");
}

// ============================================================================
// Multiple Functions Tests
// ============================================================================

TEST_CASE("E2E - Simple function call") {
    const char* source = R"(
        fun square(x: i32): i32 {
            return x * x;
        }

        fun main(): i32 {
            print(f"{square(5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "25\n");
}

TEST_CASE("E2E - Multiple functions calling each other") {
    const char* source = R"(
        fun square(x: i32): i32 {
            return x * x;
        }

        fun sum_of_squares(a: i32, b: i32): i32 {
            return square(a) + square(b);
        }

        fun main(): i32 {
            print(f"{sum_of_squares(3, 4)}");
            return 0;
        }
    )";

    // 3^2 + 4^2 = 9 + 16 = 25
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "25\n");
}

TEST_CASE("E2E - Mutual recursion") {
    const char* source = R"(
        fun is_even(n: i32): i32 {
            if (n == 0) {
                return 1;
            }
            return is_odd(n - 1);
        }

        fun is_odd(n: i32): i32 {
            if (n == 0) {
                return 0;
            }
            return is_even(n - 1);
        }

        fun main(): i32 {
            print(f"{is_even(0)}");
            print(f"{is_even(4)}");
            print(f"{is_even(7)}");
            print(f"{is_odd(5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // is_even(0)=1, is_even(4)=1, is_even(7)=0, is_odd(5)=1
    CHECK(result.stdout_output == "1\n1\n0\n1\n");
}

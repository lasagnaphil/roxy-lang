#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Recursive Function Tests
// ============================================================================

TEST_SUITE("E2E Recursion") {

    TEST_CASE_TEMPLATE("Factorial (recursive)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n1\n120\n3628800\n");
    }

    TEST_CASE_TEMPLATE("Fibonacci (recursive)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n1\n55\n610\n");
    }

    TEST_CASE_TEMPLATE("GCD (Euclidean algorithm)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // gcd(48,18)=6, gcd(100,35)=5, gcd(17,13)=1, gcd(12,12)=12
        CHECK(result.stdout_output == "6\n5\n1\n12\n");
    }

    TEST_CASE_TEMPLATE("Power function (recursive)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // 2^0=1, 2^10=1024, 3^5=243
        CHECK(result.stdout_output == "1\n1024\n243\n");
    }

    // ============================================================================
    // Multiple Functions Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Simple function call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun square(x: i32): i32 {
            return x * x;
        }

        fun main(): i32 {
            print(f"{square(5)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "25\n");
    }

    TEST_CASE_TEMPLATE("Multiple functions calling each other", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "25\n");
    }

    TEST_CASE_TEMPLATE("Mutual recursion", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // is_even(0)=1, is_even(4)=1, is_even(7)=0, is_odd(5)=1
        CHECK(result.stdout_output == "1\n1\n0\n1\n");
    }

    TEST_CASE("unbounded recursion fails gracefully (call stack overflow)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        // Unbounded recursion must hit the call-stack-depth guard in CALL and
        // return a clean error, not write past the fixed-size call-frame array.
        const char* source = R"(
        fun f(n: i32): i32 { return f(n - 1); }
        fun main(): i32 { return f(1000000); }
    )";

        auto result = VMBackend::run(source);
        CHECK_FALSE(result.success);
    }

}  // TEST_SUITE("E2E Recursion")

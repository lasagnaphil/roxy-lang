#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Float Tests
// ============================================================================

TEST_SUITE("E2E Algorithms") {

    TEST_CASE("Float arithmetic") {
        const char* source = R"(
        fun calc(): f64 {
            var a: f64 = 3.5;
            var b: f64 = 2.5;
            return a + b;
        }

        fun main(): i32 {
            return i32(calc());
        }
    )";

        Value result = compile_and_run(source, "main");
        CHECK(result.as_int == 6);
    }

    TEST_CASE("Float comparison") {
        const char* source = R"(
        fun max_float(a: f64, b: f64): f64 {
            if (a > b) {
                return a;
            }
            return b;
        }

        fun main(): i32 {
            // Pack two boolean checks into one int so a single pass exercises both branches.
            var hi: i32 = 0;
            if (max_float(3.14, 2.71) == 3.14) { hi = 1; }
            var lo: i32 = 0;
            if (max_float(1.5, 4.5) == 4.5) { lo = 1; }
            return hi * 2 + lo;
        }
    )";

        Value result = compile_and_run(source, "main");
        CHECK(result.as_int == 3);  // both branches matched
    }

    // ============================================================================
    // Complex Algorithm Tests
    // ============================================================================

    TEST_CASE("Ackermann function (deeply recursive)") {
        const char* source = R"(
        fun ackermann(m: i32, n: i32): i32 {
            if (m == 0) {
                return n + 1;
            }
            if (n == 0) {
                return ackermann(m - 1, 1);
            }
            return ackermann(m - 1, ackermann(m, n - 1));
        }

        fun main(): i32 {
            var a: i32 = ackermann(0, 0);
            var b: i32 = ackermann(1, 1);
            var c: i32 = ackermann(2, 2);
            var d: i32 = ackermann(3, 2);
            return a + b + c + d;
        }
    )";

        BumpAllocator allocator(4096);
        BCModule* module = compile(allocator, source);
        REQUIRE(module != nullptr);

        RoxyVM vm;
        VMConfig config;
        config.register_file_size = 65536;  // Need more registers for deep recursion
        config.max_call_depth = 4096;
        vm_init(&vm, config);
        vm_load_module(&vm, module);

        CHECK(vm_call(&vm, "main", {}));
        // ackermann(0,0)=1, ackermann(1,1)=3, ackermann(2,2)=7, ackermann(3,2)=29
        CHECK(vm_get_result(&vm).as_int == 1 + 3 + 7 + 29);

        vm_destroy(&vm);
        delete module;
    }

    TEST_CASE("Collatz conjecture steps") {
        const char* source = R"(
        fun collatz_steps(n: i32): i32 {
            var steps: i32 = 0;
            while (n != 1) {
                if (n % 2 == 0) {
                    n = n / 2;
                } else {
                    n = 3 * n + 1;
                }
                steps = steps + 1;
            }
            return steps;
        }

        fun main(): i32 {
            print(f"{collatz_steps(1)}");
            print(f"{collatz_steps(6)}");
            print(f"{collatz_steps(27)}");
            return 0;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // collatz_steps(1)=0, collatz_steps(6)=8, collatz_steps(27)=111
        CHECK(result.stdout_output == "0\n8\n111\n");
    }

    TEST_CASE("Prime checking") {
        const char* source = R"(
        fun is_prime(n: i32): i32 {
            if (n <= 1) {
                return 0;
            }
            if (n <= 3) {
                return 1;
            }
            if (n % 2 == 0) {
                return 0;
            }
            var i: i32 = 3;
            while (i * i <= n) {
                if (n % i == 0) {
                    return 0;
                }
                i = i + 2;
            }
            return 1;
        }

        fun main(): i32 {
            // Test primes: 2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 97
            print(f"{is_prime(2)}");
            print(f"{is_prime(3)}");
            print(f"{is_prime(5)}");
            print(f"{is_prime(7)}");
            print(f"{is_prime(11)}");
            print(f"{is_prime(97)}");
            // Test non-primes: 0, 1, 4, 6, 9, 100
            print(f"{is_prime(0)}");
            print(f"{is_prime(1)}");
            print(f"{is_prime(4)}");
            print(f"{is_prime(6)}");
            print(f"{is_prime(9)}");
            print(f"{is_prime(100)}");
            return 0;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        // primes return 1, non-primes return 0
        CHECK(result.stdout_output == "1\n1\n1\n1\n1\n1\n0\n0\n0\n0\n0\n0\n");
    }

    // ============================================================================
    // Print Tests
    // ============================================================================

    TEST_CASE("Print function") {
        const char* source = R"(
        fun main(): i32 {
            print(f"{42}");
            print(f"{123}");
            print(f"{0}");
            return 0;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n123\n0\n");
    }

}  // TEST_SUITE("E2E Algorithms")

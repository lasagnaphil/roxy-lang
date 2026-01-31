#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Float Tests
// ============================================================================

TEST_CASE("E2E - Float arithmetic") {
    const char* source = R"(
        fun calc(): f64 {
            var a: f64 = 3.5;
            var b: f64 = 2.5;
            return a + b;
        }
    )";

    Value result = compile_and_run(source, "calc");
    // With untyped registers, we need to interpret the raw bits as float
    Value float_result = Value::float_from_u64(result.as_u64());
    CHECK(float_result.as_float == doctest::Approx(6.0));
}

TEST_CASE("E2E - Float comparison") {
    const char* source = R"(
        fun max_float(a: f64, b: f64): f64 {
            if (a > b) {
                return a;
            }
            return b;
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    Value args[2] = {Value::make_float(3.14), Value::make_float(2.71)};
    CHECK(vm_call(&vm, "max_float", Span<Value>(args, 2)));
    // With untyped registers, interpret result bits as float
    Value float_result = Value::float_from_u64(vm_get_result(&vm).as_u64());
    CHECK(float_result.as_float == doctest::Approx(3.14));

    vm_destroy(&vm);
    delete module;
}

// ============================================================================
// Complex Algorithm Tests
// ============================================================================

TEST_CASE("E2E - Ackermann function (deeply recursive)") {
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
            print(ackermann(0, 0));
            print(ackermann(1, 1));
            print(ackermann(2, 2));
            print(ackermann(3, 2));
            return 0;
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
    CHECK(vm_get_result(&vm).as_int == 0);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Collatz conjecture steps") {
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
            print(collatz_steps(1));
            print(collatz_steps(6));
            print(collatz_steps(27));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // collatz_steps(1)=0, collatz_steps(6)=8, collatz_steps(27)=111
    CHECK(result.stdout_output == "0\n8\n111\n");
}

TEST_CASE("E2E - Prime checking") {
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
            print(is_prime(2));
            print(is_prime(3));
            print(is_prime(5));
            print(is_prime(7));
            print(is_prime(11));
            print(is_prime(97));
            // Test non-primes: 0, 1, 4, 6, 9, 100
            print(is_prime(0));
            print(is_prime(1));
            print(is_prime(4));
            print(is_prime(6));
            print(is_prime(9));
            print(is_prime(100));
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

TEST_CASE("E2E - Print function") {
    const char* source = R"(
        fun main(): i32 {
            print(42);
            print(123);
            print(0);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n123\n0\n");
}

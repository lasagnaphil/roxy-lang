#include "roxy/core/doctest/doctest.h"
#include "e2e_test_helpers.hpp"

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

    Value result = compile_and_run(source, StringView("calc"));
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
    CHECK(vm_call(&vm, StringView("max_float"), Span<Value>(args, 2)));
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

    // A(0, 0) = 1
    Value args1[2] = {Value::make_int(0), Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("ackermann"), Span<Value>(args1, 2)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // A(1, 1) = 3
    Value args2[2] = {Value::make_int(1), Value::make_int(1)};
    CHECK(vm_call(&vm, StringView("ackermann"), Span<Value>(args2, 2)));
    CHECK(vm_get_result(&vm).as_int == 3);

    // A(2, 2) = 7
    Value args3[2] = {Value::make_int(2), Value::make_int(2)};
    CHECK(vm_call(&vm, StringView("ackermann"), Span<Value>(args3, 2)));
    CHECK(vm_get_result(&vm).as_int == 7);

    // A(3, 2) = 29
    Value args4[2] = {Value::make_int(3), Value::make_int(2)};
    CHECK(vm_call(&vm, StringView("ackermann"), Span<Value>(args4, 2)));
    CHECK(vm_get_result(&vm).as_int == 29);

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
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // collatz_steps(1) = 0
    Value args1[1] = {Value::make_int(1)};
    CHECK(vm_call(&vm, StringView("collatz_steps"), Span<Value>(args1, 1)));
    CHECK(vm_get_result(&vm).as_int == 0);

    // collatz_steps(6) = 8 (6 -> 3 -> 10 -> 5 -> 16 -> 8 -> 4 -> 2 -> 1)
    Value args6[1] = {Value::make_int(6)};
    CHECK(vm_call(&vm, StringView("collatz_steps"), Span<Value>(args6, 1)));
    CHECK(vm_get_result(&vm).as_int == 8);

    // collatz_steps(27) = 111
    Value args27[1] = {Value::make_int(27)};
    CHECK(vm_call(&vm, StringView("collatz_steps"), Span<Value>(args27, 1)));
    CHECK(vm_get_result(&vm).as_int == 111);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Prime checking") {
    const char* source = R"(
        fun is_prime(n: i32): bool {
            if (n <= 1) {
                return false;
            }
            if (n <= 3) {
                return true;
            }
            if (n % 2 == 0) {
                return false;
            }
            var i: i32 = 3;
            while (i * i <= n) {
                if (n % i == 0) {
                    return false;
                }
                i = i + 2;
            }
            return true;
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // Test primes
    i32 primes[] = {2, 3, 5, 7, 11, 13, 17, 19, 23, 29, 97};
    for (i32 p : primes) {
        Value args[1] = {Value::make_int(p)};
        CHECK(vm_call(&vm, StringView("is_prime"), Span<Value>(args, 1)));
        CHECK(vm_get_result(&vm).as_bool == true);
    }

    // Test non-primes
    i32 non_primes[] = {0, 1, 4, 6, 8, 9, 10, 12, 15, 21, 100};
    for (i32 np : non_primes) {
        Value args[1] = {Value::make_int(np)};
        CHECK(vm_call(&vm, StringView("is_prime"), Span<Value>(args, 1)));
        CHECK(vm_get_result(&vm).as_bool == false);
    }

    vm_destroy(&vm);
    delete module;
}

// ============================================================================
// Print Tests
// ============================================================================

TEST_CASE("E2E - Print function") {
    // Test that print() compiles and runs without error
    // (output goes to stdout, we just verify it doesn't crash)
    const char* source = R"(
        fun test_print(): i32 {
            print(42);
            print(123);
            print(0);
            return 0;
        }
    )";

    Value result = compile_and_run(source, StringView("test_print"));
    CHECK(result.is_int());
    CHECK(result.as_int == 0);
}

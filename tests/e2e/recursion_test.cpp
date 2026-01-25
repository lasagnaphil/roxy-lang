#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

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
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // factorial(0) = 1
    Value args0[1] = {Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("factorial"), Span<Value>(args0, 1)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // factorial(1) = 1
    Value args1[1] = {Value::make_int(1)};
    CHECK(vm_call(&vm, StringView("factorial"), Span<Value>(args1, 1)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // factorial(5) = 120
    Value args5[1] = {Value::make_int(5)};
    CHECK(vm_call(&vm, StringView("factorial"), Span<Value>(args5, 1)));
    CHECK(vm_get_result(&vm).as_int == 120);

    // factorial(10) = 3628800
    Value args10[1] = {Value::make_int(10)};
    CHECK(vm_call(&vm, StringView("factorial"), Span<Value>(args10, 1)));
    CHECK(vm_get_result(&vm).as_int == 3628800);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Fibonacci (recursive)") {
    const char* source = R"(
        fun fib(n: i32): i32 {
            if (n <= 1) {
                return n;
            }
            return fib(n - 1) + fib(n - 2);
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // fib(0) = 0
    Value args0[1] = {Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("fib"), Span<Value>(args0, 1)));
    CHECK(vm_get_result(&vm).as_int == 0);

    // fib(1) = 1
    Value args1[1] = {Value::make_int(1)};
    CHECK(vm_call(&vm, StringView("fib"), Span<Value>(args1, 1)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // fib(10) = 55
    Value args10[1] = {Value::make_int(10)};
    CHECK(vm_call(&vm, StringView("fib"), Span<Value>(args10, 1)));
    CHECK(vm_get_result(&vm).as_int == 55);

    // fib(15) = 610
    Value args15[1] = {Value::make_int(15)};
    CHECK(vm_call(&vm, StringView("fib"), Span<Value>(args15, 1)));
    CHECK(vm_get_result(&vm).as_int == 610);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - GCD (Euclidean algorithm)") {
    const char* source = R"(
        fun gcd(a: i32, b: i32): i32 {
            if (b == 0) {
                return a;
            }
            return gcd(b, a % b);
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // gcd(48, 18) = 6
    Value args1[2] = {Value::make_int(48), Value::make_int(18)};
    CHECK(vm_call(&vm, StringView("gcd"), Span<Value>(args1, 2)));
    CHECK(vm_get_result(&vm).as_int == 6);

    // gcd(100, 35) = 5
    Value args2[2] = {Value::make_int(100), Value::make_int(35)};
    CHECK(vm_call(&vm, StringView("gcd"), Span<Value>(args2, 2)));
    CHECK(vm_get_result(&vm).as_int == 5);

    // gcd(17, 13) = 1 (coprime)
    Value args3[2] = {Value::make_int(17), Value::make_int(13)};
    CHECK(vm_call(&vm, StringView("gcd"), Span<Value>(args3, 2)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // gcd(12, 12) = 12
    Value args4[2] = {Value::make_int(12), Value::make_int(12)};
    CHECK(vm_call(&vm, StringView("gcd"), Span<Value>(args4, 2)));
    CHECK(vm_get_result(&vm).as_int == 12);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Power function (recursive)") {
    const char* source = R"(
        fun power(base: i32, exp: i32): i32 {
            if (exp == 0) {
                return 1;
            }
            return base * power(base, exp - 1);
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // 2^0 = 1
    Value args1[2] = {Value::make_int(2), Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("power"), Span<Value>(args1, 2)));
    CHECK(vm_get_result(&vm).as_int == 1);

    // 2^10 = 1024
    Value args2[2] = {Value::make_int(2), Value::make_int(10)};
    CHECK(vm_call(&vm, StringView("power"), Span<Value>(args2, 2)));
    CHECK(vm_get_result(&vm).as_int == 1024);

    // 3^5 = 243
    Value args3[2] = {Value::make_int(3), Value::make_int(5)};
    CHECK(vm_call(&vm, StringView("power"), Span<Value>(args3, 2)));
    CHECK(vm_get_result(&vm).as_int == 243);

    vm_destroy(&vm);
    delete module;
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
            return square(5);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 25);
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
            return sum_of_squares(3, 4);
        }
    )";

    // 3^2 + 4^2 = 9 + 16 = 25
    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 25);
}

TEST_CASE("E2E - Mutual recursion") {
    const char* source = R"(
        fun is_even(n: i32): bool {
            if (n == 0) {
                return true;
            }
            return is_odd(n - 1);
        }

        fun is_odd(n: i32): bool {
            if (n == 0) {
                return false;
            }
            return is_even(n - 1);
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // is_even(0) = true
    Value args0[1] = {Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("is_even"), Span<Value>(args0, 1)));
    CHECK(vm_get_result(&vm).as_bool == true);

    // is_even(4) = true
    Value args4[1] = {Value::make_int(4)};
    CHECK(vm_call(&vm, StringView("is_even"), Span<Value>(args4, 1)));
    CHECK(vm_get_result(&vm).as_bool == true);

    // is_even(7) = false
    Value args7[1] = {Value::make_int(7)};
    CHECK(vm_call(&vm, StringView("is_even"), Span<Value>(args7, 1)));
    CHECK(vm_get_result(&vm).as_bool == false);

    // is_odd(5) = true
    Value args5[1] = {Value::make_int(5)};
    CHECK(vm_call(&vm, StringView("is_odd"), Span<Value>(args5, 1)));
    CHECK(vm_get_result(&vm).as_bool == true);

    vm_destroy(&vm);
    delete module;
}

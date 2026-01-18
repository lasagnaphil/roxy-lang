#include "roxy/core/doctest/doctest.h"
#include "e2e_test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Basic Tests
// ============================================================================

TEST_CASE("E2E - Return constant") {
    const char* source = R"(
        fun answer(): i32 {
            return 42;
        }
    )";

    Value result = compile_and_run(source, StringView("answer"));
    CHECK(result.is_int());
    CHECK(result.as_int == 42);
}

TEST_CASE("E2E - Arithmetic expressions") {
    SUBCASE("Addition") {
        const char* source = R"(
            fun calc(): i32 {
                return 10 + 32;
            }
        )";
        Value result = compile_and_run(source, StringView("calc"));
        CHECK(result.as_int == 42);
    }

    SUBCASE("Complex expression") {
        const char* source = R"(
            fun calc(): i32 {
                return 1 + 2 * 3 + 4 * 5;
            }
        )";
        // 1 + 6 + 20 = 27
        Value result = compile_and_run(source, StringView("calc"));
        CHECK(result.as_int == 27);
    }

    SUBCASE("Parentheses") {
        const char* source = R"(
            fun calc(): i32 {
                return (1 + 2) * (3 + 4);
            }
        )";
        // 3 * 7 = 21
        Value result = compile_and_run(source, StringView("calc"));
        CHECK(result.as_int == 21);
    }

    SUBCASE("Division and modulo") {
        const char* source = R"(
            fun calc(): i32 {
                return 100 / 3;
            }
        )";
        Value result = compile_and_run(source, StringView("calc"));
        CHECK(result.as_int == 33);
    }

    SUBCASE("Negation") {
        const char* source = R"(
            fun calc(): i32 {
                return -42;
            }
        )";
        Value result = compile_and_run(source, StringView("calc"));
        CHECK(result.as_int == -42);
    }
}

TEST_CASE("E2E - Local variables") {
    const char* source = R"(
        fun calc(): i32 {
            var a: i32 = 10;
            var b: i32 = 20;
            var c: i32 = a + b;
            return c;
        }
    )";

    Value result = compile_and_run(source, StringView("calc"));
    CHECK(result.as_int == 30);
}

TEST_CASE("E2E - Function parameters") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    Value args[2] = {Value::make_int(17), Value::make_int(25)};
    CHECK(vm_call(&vm, StringView("add"), Span<Value>(args, 2)));
    CHECK(vm_get_result(&vm).as_int == 42);

    vm_destroy(&vm);
    delete module;
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
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // Test positive
    Value args1[1] = {Value::make_int(42)};
    CHECK(vm_call(&vm, StringView("abs"), Span<Value>(args1, 1)));
    CHECK(vm_get_result(&vm).as_int == 42);

    // Test negative
    Value args2[1] = {Value::make_int(-42)};
    CHECK(vm_call(&vm, StringView("abs"), Span<Value>(args2, 1)));
    CHECK(vm_get_result(&vm).as_int == 42);

    // Test zero
    Value args3[1] = {Value::make_int(0)};
    CHECK(vm_call(&vm, StringView("abs"), Span<Value>(args3, 1)));
    CHECK(vm_get_result(&vm).as_int == 0);

    vm_destroy(&vm);
    delete module;
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
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    Value args1[2] = {Value::make_int(10), Value::make_int(5)};
    CHECK(vm_call(&vm, StringView("max"), Span<Value>(args1, 2)));
    CHECK(vm_get_result(&vm).as_int == 10);

    Value args2[2] = {Value::make_int(3), Value::make_int(7)};
    CHECK(vm_call(&vm, StringView("max"), Span<Value>(args2, 2)));
    CHECK(vm_get_result(&vm).as_int == 7);

    Value args3[2] = {Value::make_int(5), Value::make_int(5)};
    CHECK(vm_call(&vm, StringView("max"), Span<Value>(args3, 2)));
    CHECK(vm_get_result(&vm).as_int == 5);

    vm_destroy(&vm);
    delete module;
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
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // sum(1..10) = 55
    Value args[1] = {Value::make_int(10)};
    CHECK(vm_call(&vm, StringView("sum_to_n"), Span<Value>(args, 1)));
    CHECK(vm_get_result(&vm).as_int == 55);

    // sum(1..100) = 5050
    Value args2[1] = {Value::make_int(100)};
    CHECK(vm_call(&vm, StringView("sum_to_n"), Span<Value>(args2, 1)));
    CHECK(vm_get_result(&vm).as_int == 5050);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - For loop") {
    const char* source = R"(
        fun sum_to_n(n: i32): i32 {
            var sum: i32 = 0;
            for (var i: i32 = 1; i <= n; i = i + 1) {
                sum = sum + i;
            }
            return sum;
        }
    )";

    BumpAllocator allocator(4096);
    BCModule* module = compile(allocator, source);
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    Value args[1] = {Value::make_int(10)};
    CHECK(vm_call(&vm, StringView("sum_to_n"), Span<Value>(args, 1)));
    CHECK(vm_get_result(&vm).as_int == 55);

    vm_destroy(&vm);
    delete module;
}

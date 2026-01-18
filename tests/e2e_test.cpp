#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

using namespace rx;

// Helper to compile Roxy source to bytecode module
// Set debug=true to print generated IR for debugging
static BCModule* compile(BumpAllocator& allocator, const char* source, bool debug = false) {
    u32 len = 0;
    while (source[len]) len++;

    // Create type cache and registry
    TypeCache types(allocator);
    NativeRegistry registry(allocator, types);
    register_builtin_natives(registry);

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator, &registry);
    if (!analyzer.analyze(program)) {
        return nullptr;
    }

    IRBuilder ir_builder(allocator, analyzer.types(), &registry);
    IRModule* ir_module = ir_builder.build(program);
    if (!ir_module) {
        return nullptr;
    }

    if (debug) {
        Vector<char> ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        printf("=== IR ===\n%s\n", ir_str.data());
    }

    BytecodeBuilder bc_builder;
    BCModule* module = bc_builder.build(ir_module);
    if (module) {
        // Register native functions with the module for runtime
        registry.apply_to_module(module);
    }
    return module;
}

// Helper to compile and run, returning result
static Value compile_and_run(const char* source, StringView func_name, Span<Value> args = {}) {
    BumpAllocator allocator(8192);
    BCModule* module = compile(allocator, source);
    if (!module) {
        return Value::make_null();
    }

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    if (!vm_call(&vm, func_name, args)) {
        vm_destroy(&vm);
        delete module;
        return Value::make_null();
    }

    Value result = vm_get_result(&vm);
    vm_destroy(&vm);
    delete module;
    return result;
}

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

// ============================================================================
// Array Tests
// ============================================================================

TEST_CASE("E2E - Array basic operations") {
    const char* source = R"(
        fun test_array(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return arr[0] + arr[1] + arr[2];
        }
    )";

    Value result = compile_and_run(source, StringView("test_array"));
    CHECK(result.is_int());
    CHECK(result.as_int == 60);  // 10 + 20 + 30
}

TEST_CASE("E2E - Array length") {
    const char* source = R"(
        fun test_len(): i32 {
            var arr: i32[] = array_new_int(7);
            print(array_len(arr));
            return array_len(arr);
        }
    )";

    Value result = compile_and_run(source, StringView("test_len"));
    CHECK(result.is_int());
    CHECK(result.as_int == 7);
}

TEST_CASE("E2E - Array with loop") {
    const char* source = R"(
        fun sum_array(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 1;
            arr[1] = 2;
            arr[2] = 3;
            arr[3] = 4;
            arr[4] = 5;

            var sum: i32 = 0;
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
                sum = sum + arr[i];
            }
            return sum;
        }
    )";

    Value result = compile_and_run(source, StringView("sum_array"));
    CHECK(result.is_int());
    CHECK(result.as_int == 15);  // 1 + 2 + 3 + 4 + 5
}

TEST_CASE("E2E - Array swap") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun test_swap(): i32 {
            var arr: i32[] = array_new_int(3);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            swap(arr, 0, 2);
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return arr[0] * 100 + arr[1] * 10 + arr[2];
        }
    )";

    Value result = compile_and_run(source, StringView("test_swap"));
    CHECK(result.is_int());
    CHECK(result.as_int == 3210);  // arr[0]=30, arr[1]=20, arr[2]=10 -> 30*100 + 20*10 + 10 = 3210
}

TEST_CASE("E2E - Quicksort") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun partition(arr: i32[], low: i32, high: i32): i32 {
            var pivot: i32 = arr[high];
            var i: i32 = low - 1;
            for (var j: i32 = low; j < high; j = j + 1) {
                if (arr[j] <= pivot) {
                    i = i + 1;
                    swap(arr, i, j);
                }
            }
            swap(arr, i + 1, high);
            return i + 1;
        }

        fun quicksort(arr: i32[], low: i32, high: i32) {
            if (low < high) {
                var pi: i32 = partition(arr, low, high);
                quicksort(arr, low, pi - 1);
                quicksort(arr, pi + 1, high);
            }
        }

        fun test_quicksort(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 5;
            arr[1] = 2;
            arr[2] = 8;
            arr[3] = 1;
            arr[4] = 9;

            quicksort(arr, 0, array_len(arr) - 1);

            // Print sorted array
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
            }

            return arr[0];
        }
    )";

    Value result = compile_and_run(source, StringView("test_quicksort"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // First element should be 1 after sorting
}

TEST_CASE("E2E - Quicksort verify all elements") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun partition(arr: i32[], low: i32, high: i32): i32 {
            var pivot: i32 = arr[high];
            var i: i32 = low - 1;
            for (var j: i32 = low; j < high; j = j + 1) {
                if (arr[j] <= pivot) {
                    i = i + 1;
                    swap(arr, i, j);
                }
            }
            swap(arr, i + 1, high);
            return i + 1;
        }

        fun quicksort(arr: i32[], low: i32, high: i32) {
            if (low < high) {
                var pi: i32 = partition(arr, low, high);
                quicksort(arr, low, pi - 1);
                quicksort(arr, pi + 1, high);
            }
        }

        fun verify_sorted(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 5;
            arr[1] = 2;
            arr[2] = 8;
            arr[3] = 1;
            arr[4] = 9;

            quicksort(arr, 0, array_len(arr) - 1);

            // Print sorted array
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
            }

            // Return encoded sorted array for verification
            return arr[0] + arr[1] * 10 + arr[2] * 100 + arr[3] * 1000 + arr[4] * 10000;
        }
    )";

    Value result = compile_and_run(source, StringView("verify_sorted"));
    CHECK(result.is_int());
    CHECK(result.as_int == 98521);  // [1, 2, 5, 8, 9] encoded
}

// ============================================================================
// Struct Tests
// ============================================================================

TEST_CASE("E2E - Basic struct field access") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 10;
            p.y = 20;
            return p.x + p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 30);  // 10 + 20
}

TEST_CASE("E2E - Struct with 64-bit field") {
    const char* source = R"(
        struct Data {
            a: i32;
            b: i64;
        }

        fun main(): i64 {
            var d: Data;
            d.a = 10;
            d.b = 100000000000;
            return d.a + d.b;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.as_int == 100000000010);  // 10 + 100000000000
}

TEST_CASE("E2E - Multiple struct variables") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p1: Point;
            var p2: Point;
            p1.x = 1;
            p1.y = 2;
            p2.x = 3;
            p2.y = 4;
            return p1.x + p1.y + p2.x + p2.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 10);  // 1 + 2 + 3 + 4
}

TEST_CASE("E2E - Struct field assignment from expression") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 3 * 4;
            p.y = p.x + 5;
            return p.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 17);  // 12 + 5
}

TEST_CASE("E2E - Struct with float fields") {
    const char* source = R"(
        struct Vec2 {
            x: f64;
            y: f64;
        }

        fun main(): f64 {
            var v: Vec2;
            v.x = 1.5;
            v.y = 2.5;
            return v.x + v.y;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    Value float_result = Value::float_from_u64(result.as_u64());
    CHECK(float_result.as_float == doctest::Approx(4.0));  // 1.5 + 2.5
}

TEST_CASE("E2E - Struct in conditional") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point;
            p.x = 10;
            p.y = 5;
            if (p.x > p.y) {
                return p.x;
            } else {
                return p.y;
            }
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 10);
}

TEST_CASE("E2E - Struct in loop") {
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun main(): i32 {
            var c: Counter;
            c.value = 0;
            for (var i: i32 = 0; i < 10; i = i + 1) {
                c.value = c.value + i;
            }
            return c.value;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 45);  // 0 + 1 + 2 + ... + 9
}

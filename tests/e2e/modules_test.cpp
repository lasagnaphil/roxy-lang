#include "roxy/core/doctest/doctest.h"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/compiler/compiler.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

namespace rx {

// Native math functions for testing
static i32 math_add(i32 a, i32 b) { return a + b; }
static i32 math_mul(i32 a, i32 b) { return a * b; }
static i32 math_square(i32 x) { return x * x; }
static i32 math_negate(i32 x) { return -x; }

// Helper to compile and run with module support
struct ModuleTestContext {
    BumpAllocator allocator;
    TypeCache types;
    NativeRegistry math_natives;

    ModuleTestContext()
        : allocator(16384)
        , types(allocator)
        , math_natives(allocator, types)
    {
        // Register math module natives
        math_natives.bind<math_add>("add");
        math_natives.bind<math_mul>("mul");
        math_natives.bind<math_square>("square");
        math_natives.bind<math_negate>("negate");
    }

    i64 compile_and_run(const char* source, bool debug = false) {
        u32 len = static_cast<u32>(strlen(source));

        // Use Compiler class - it handles builtin prelude automatically
        Compiler compiler(allocator);
        compiler.add_native_registry("math", &math_natives);
        compiler.add_source("main", source, len);

        BCModule* module = compiler.compile();
        if (!module) {
            if (debug) {
                printf("Compilation errors:\n");
                for (const char* err : compiler.errors()) {
                    printf("  %s\n", err);
                }
            }
            return -999;
        }

        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, module);

        if (!vm_call(&vm, "main", {})) {
            vm_destroy(&vm);
            delete module;
            return -995;
        }

        Value result = vm_get_result(&vm);
        vm_destroy(&vm);
        delete module;
        return result.as_int;
    }

    bool has_error(const char* source, const char* expected_error_substr = nullptr) {
        u32 len = static_cast<u32>(strlen(source));

        Compiler compiler(allocator);
        compiler.add_native_registry("math", &math_natives);
        compiler.add_source("main", source, len);

        BCModule* module = compiler.compile();
        if (module) {
            delete module;
            return false;  // No error
        }

        if (expected_error_substr) {
            for (const char* err : compiler.errors()) {
                if (strstr(err, expected_error_substr) != nullptr) {
                    return true;
                }
            }
            return false;  // Error but not the expected one
        }

        return true;  // Has error
    }
};

TEST_CASE("E2E - Module: from import basic native function") {
    ModuleTestContext ctx;

    const char* source = R"(
        from math import add;

        fun main(): i32 {
            return add(3, 4);
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == 7);
}

TEST_CASE("E2E - Module: from import multiple functions") {
    ModuleTestContext ctx;

    const char* source = R"(
        from math import add, mul;

        fun main(): i32 {
            var a: i32 = add(2, 3);
            var b: i32 = mul(a, 4);
            return b;
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == 20);  // (2+3) * 4 = 20
}

TEST_CASE("E2E - Module: from import with alias") {
    ModuleTestContext ctx;

    const char* source = R"(
        from math import add as plus;

        fun main(): i32 {
            return plus(10, 20);
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == 30);
}

TEST_CASE("E2E - Module: import for qualified access") {
    ModuleTestContext ctx;

    const char* source = R"(
        import math;

        fun main(): i32 {
            return math.square(5);
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == 25);
}

TEST_CASE("E2E - Module: qualified access with multiple calls") {
    ModuleTestContext ctx;

    const char* source = R"(
        import math;

        fun main(): i32 {
            var a: i32 = math.add(1, 2);
            var b: i32 = math.mul(a, 3);
            return math.square(b);
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == 81);  // ((1+2)*3)^2 = 9^2 = 81
}

TEST_CASE("E2E - Module: mix from import and qualified access") {
    ModuleTestContext ctx;

    const char* source = R"(
        import math;
        from math import negate;

        fun main(): i32 {
            var a: i32 = math.add(5, 5);
            return negate(a);
        }
    )";

    i64 result = ctx.compile_and_run(source);
    CHECK(result == -10);
}

TEST_CASE("E2E - Module: error on unknown module") {
    ModuleTestContext ctx;

    const char* source = R"(
        from unknown_module import func;

        fun main(): i32 {
            return 0;
        }
    )";

    CHECK(ctx.has_error(source, "unknown module"));
}

TEST_CASE("E2E - Module: error on unknown export") {
    ModuleTestContext ctx;

    const char* source = R"(
        from math import nonexistent;

        fun main(): i32 {
            return 0;
        }
    )";

    CHECK(ctx.has_error(source, "no export"));
}

TEST_CASE("E2E - Module: error on duplicate import") {
    ModuleTestContext ctx;

    const char* source = R"(
        from math import add;
        from math import add;

        fun main(): i32 {
            return 0;
        }
    )";

    CHECK(ctx.has_error(source, "redefinition"));
}

// =============================================================================
// Script Module Tests using Compiler class
// =============================================================================

TEST_CASE("E2E - Compiler: single module compilation") {
    BumpAllocator allocator(16384);

    const char* main_source = R"(
        pub fun add(a: i32, b: i32): i32 {
            return a + b;
        }

        fun main(): i32 {
            return add(10, 20);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);
    REQUIRE(!compiler.has_errors());

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 30);

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Compiler: two module compilation with import") {
    BumpAllocator allocator(16384);

    const char* utils_source = R"(
        pub fun double(x: i32): i32 {
            return x * 2;
        }

        pub fun triple(x: i32): i32 {
            return x * 3;
        }
    )";

    const char* main_source = R"(
        import utils;

        fun main(): i32 {
            return utils.double(5) + utils.triple(3);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("utils", utils_source, static_cast<u32>(strlen(utils_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);
    REQUIRE(!compiler.has_errors());

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 19);  // double(5)=10 + triple(3)=9 = 19

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Compiler: circular import detection") {
    BumpAllocator allocator(16384);

    const char* module_a = R"(
        import b;
        pub fun foo(): i32 { return 1; }
    )";

    const char* module_b = R"(
        import a;
        pub fun bar(): i32 { return 2; }
    )";

    Compiler compiler(allocator);
    compiler.add_source("a", module_a, static_cast<u32>(strlen(module_a)));
    compiler.add_source("b", module_b, static_cast<u32>(strlen(module_b)));

    BCModule* module = compiler.compile();
    CHECK(module == nullptr);
    CHECK(compiler.has_errors());

    // Check that the error mentions "circular" or "cycle"
    bool found_cycle_error = false;
    for (const char* err : compiler.errors()) {
        if (strstr(err, "Circular") || strstr(err, "cycle")) {
            found_cycle_error = true;
            break;
        }
    }
    CHECK(found_cycle_error);
}

TEST_CASE("E2E - Compiler: from import with script module") {
    BumpAllocator allocator(16384);

    const char* math_source = R"(
        pub fun square(x: i32): i32 {
            return x * x;
        }

        pub fun cube(x: i32): i32 {
            return x * x * x;
        }
    )";

    const char* main_source = R"(
        from mymath import square, cube;

        fun main(): i32 {
            return square(3) + cube(2);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("mymath", math_source, static_cast<u32>(strlen(math_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);
    REQUIRE(!compiler.has_errors());

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 17);  // square(3)=9 + cube(2)=8 = 17

    vm_destroy(&vm);
    delete module;
}

} // namespace rx

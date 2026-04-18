#include "roxy/core/doctest/doctest.h"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/compiler/compiler.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

namespace rx {

// Native math functions for testing (all take RoxyVM* as first parameter)
static i32 math_add(RoxyVM* vm, i32 a, i32 b) { (void)vm; return a + b; }
static i32 math_mul(RoxyVM* vm, i32 a, i32 b) { (void)vm; return a * b; }
static i32 math_square(RoxyVM* vm, i32 x) { (void)vm; return x * x; }
static i32 math_negate(RoxyVM* vm, i32 x) { (void)vm; return -x; }

// Helper to compile and run with module support
struct ModuleTestContext {
    BumpAllocator allocator;
    TypeEnv type_env;
    NativeRegistry math_natives;

    ModuleTestContext()
        : allocator(16384)
        , type_env(allocator)
        , math_natives(allocator, type_env.types())
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

// Note: Cross-module struct visibility tests require struct exports to be implemented.
// For now, we test same-module visibility which is the most common case.

TEST_CASE("E2E - Compiler: struct field visibility - same module private access allowed") {
    BumpAllocator allocator(16384);

    // Private fields should be accessible within the same module
    const char* main_source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            return p.x + p.y;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

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

TEST_CASE("E2E - Compiler: struct field visibility - same module public fields work") {
    BumpAllocator allocator(16384);

    // Public fields should also work
    const char* main_source = R"(
        struct Point {
            pub x: i32;
            pub y: i32;
        }

        fun main(): i32 {
            var p: Point = Point { x = 5, y = 15 };
            return p.x + p.y;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 20);

    vm_destroy(&vm);
    delete module;
}

// =============================================================================
// Nested Module Path Tests
// =============================================================================

TEST_CASE("E2E - Module: nested module path import") {
    BumpAllocator allocator(16384);

    // Use exactly the same function structure as working test
    const char* vec2_source = R"(
        pub fun double(x: i32): i32 {
            return x * 2;
        }

        pub fun triple(x: i32): i32 {
            return x * 3;
        }
    )";

    const char* main_source = R"(
        import math.vec2;

        fun main(): i32 {
            return vec2.double(5) + vec2.triple(3);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("math.vec2", vec2_source, static_cast<u32>(strlen(vec2_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

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

TEST_CASE("E2E - Module: nested module path with multiple calls") {
    BumpAllocator allocator(16384);

    const char* vec2_source = R"(
        pub fun double(x: i32): i32 {
            return x * 2;
        }

        pub fun triple(x: i32): i32 {
            return x * 3;
        }
    )";

    const char* main_source = R"(
        import math.vec2;

        fun main(): i32 {
            var a: i32 = vec2.double(2) + vec2.triple(3);
            return a;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("math.vec2", vec2_source, static_cast<u32>(strlen(vec2_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 13);  // double(2)=4 + triple(3)=9 = 13

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Module: deeply nested path") {
    BumpAllocator allocator(16384);

    const char* collision_source = R"(
        pub fun check(x: i32): i32 {
            return x * 2;
        }

        pub fun verify(x: i32): i32 {
            return x * 3;
        }
    )";

    const char* main_source = R"(
        import game.physics.collision;

        fun main(): i32 {
            return collision.check(5) + collision.verify(2);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("game.physics.collision", collision_source, static_cast<u32>(strlen(collision_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 16);  // check(5)=10 + verify(2)=6 = 16

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Module: from import with nested path") {
    BumpAllocator allocator(16384);

    const char* vec2_source = R"(
        pub fun double(x: i32): i32 {
            return x * 2;
        }

        pub fun triple(x: i32): i32 {
            return x * 3;
        }
    )";

    const char* main_source = R"(
        from math.vec2 import double, triple;

        fun main(): i32 {
            return double(5) + triple(3);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("math.vec2", vec2_source, static_cast<u32>(strlen(vec2_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

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

TEST_CASE("E2E - Module: single-level import unchanged") {
    BumpAllocator allocator(16384);

    // Exactly match working test structure
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

TEST_CASE("E2E - Module: cross-module call result in variable") {
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
            var a: i32 = utils.double(2);
            var b: i32 = utils.triple(a);
            return b;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("utils", utils_source, static_cast<u32>(strlen(utils_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    REQUIRE(module != nullptr);

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool success = vm_call(&vm, "main", {});
    REQUIRE(success);

    Value result = vm_get_result(&vm);
    CHECK(result.as_int == 12);  // triple(double(2)) = triple(4) = 12

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Module: error on unknown nested module") {
    BumpAllocator allocator(16384);

    const char* main_source = R"(
        import math.nonexistent;

        fun main(): i32 {
            return 0;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    CHECK(module == nullptr);
    CHECK(compiler.has_errors());

    // Check that error mentions the unknown module
    bool found_error = false;
    for (const char* err : compiler.errors()) {
        if (strstr(err, "unknown module") && strstr(err, "math.nonexistent")) {
            found_error = true;
            break;
        }
    }
    CHECK(found_error);
}

// =============================================================================
// Tagged union variant field visibility across modules
// =============================================================================

TEST_CASE("E2E - Compiler: variant field visibility - cross-module private read rejected") {
    BumpAllocator allocator(16384);

    const char* lib_source = R"(
        pub enum Kind { Fire, Ice }

        pub struct Skill {
            when kind: Kind {
                case Fire:
                    burn: i32;
                case Ice:
                    slow: i32;
            }
        }

        pub fun make_fire(amount: i32): Skill {
            return Skill { kind = Kind::Fire, burn = amount };
        }
    )";

    const char* main_source = R"(
        from lib import Skill, Kind, make_fire;

        fun main(): i32 {
            var s: Skill = make_fire(7);
            when s.kind {
                case Fire:
                    return s.burn;
                case Ice:
                    return 0;
            }
            return -1;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("lib", lib_source, static_cast<u32>(strlen(lib_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    CHECK(module == nullptr);
    CHECK(compiler.has_errors());

    bool found_error = false;
    for (const char* err : compiler.errors()) {
        if (strstr(err, "variant field") && strstr(err, "burn") && strstr(err, "private")) {
            found_error = true;
            break;
        }
    }
    CHECK(found_error);
}

TEST_CASE("E2E - Compiler: variant field visibility - cross-module private init rejected") {
    BumpAllocator allocator(16384);

    const char* lib_source = R"(
        pub enum Kind { Fire, Ice }

        pub struct Skill {
            when kind: Kind {
                case Fire:
                    burn: i32;
                case Ice:
                    slow: i32;
            }
        }
    )";

    const char* main_source = R"(
        from lib import Skill, Kind;

        fun main(): i32 {
            var s: Skill = Skill { kind = Kind::Fire, burn = 7 };
            return 0;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("lib", lib_source, static_cast<u32>(strlen(lib_source)));
    compiler.add_source("main", main_source, static_cast<u32>(strlen(main_source)));

    BCModule* module = compiler.compile();
    CHECK(module == nullptr);
    CHECK(compiler.has_errors());

    bool found_error = false;
    for (const char* err : compiler.errors()) {
        if (strstr(err, "variant field") && strstr(err, "burn") && strstr(err, "private")) {
            found_error = true;
            break;
        }
    }
    CHECK(found_error);
}

TEST_CASE("E2E - Compiler: variant field visibility - cross-module pub read allowed") {
    BumpAllocator allocator(16384);

    const char* lib_source = R"(
        pub enum Kind { Fire, Ice }

        pub struct Skill {
            when kind: Kind {
                case Fire:
                    pub burn: i32;
                case Ice:
                    pub slow: i32;
            }
        }
    )";

    const char* main_source = R"(
        from lib import Skill, Kind;

        fun main(): i32 {
            var s: Skill = Skill { kind = Kind::Fire, burn = 11 };
            when s.kind {
                case Fire:
                    return s.burn;
                case Ice:
                    return 0;
            }
            return -1;
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("lib", lib_source, static_cast<u32>(strlen(lib_source)));
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
    CHECK(result.as_int == 11);

    vm_destroy(&vm);
    delete module;
}

// =============================================================================
// Module-local function name scoping (non-pub names must not leak across modules)
// =============================================================================

TEST_CASE("E2E - Compiler: non-pub function names do not collide across modules") {
    BumpAllocator allocator(16384);

    // Both modules define a non-pub `aux`. Module A's `run` calls its own `aux`.
    // Pre-fix: B's `aux` overwrote A's in the global function index; A's `run`
    // dispatched to B's body. With module-local mangling, A's `aux` becomes
    // `a::aux` and resolves correctly.
    const char* a_source = R"(
        pub fun run(): i32 {
            return aux();
        }

        fun aux(): i32 {
            return 1;
        }
    )";

    const char* b_source = R"(
        import a;

        fun aux(): i32 {
            return 2;
        }

        pub fun run_b(): i32 {
            return aux();
        }
    )";

    const char* main_source = R"(
        from a import run;
        from b import run_b;

        fun main(): i32 {
            return run() * 10 + run_b();
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("a", a_source, static_cast<u32>(strlen(a_source)));
    compiler.add_source("b", b_source, static_cast<u32>(strlen(b_source)));
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
    CHECK(result.as_int == 12);  // run()=1 * 10 + run_b()=2 = 12

    vm_destroy(&vm);
    delete module;
}

TEST_CASE("E2E - Compiler: non-pub functions with mismatched signatures do not cross modules") {
    BumpAllocator allocator(16384);

    // Pre-fix this would dispatch A's zero-arg `helper()` call to B's two-arg
    // `helper(i32, i32)`, tripping the arg_count == param_count assertion in
    // the interpreter. Post-fix, each module's `helper` is module-scoped.
    const char* a_source = R"(
        pub fun answer(): i32 {
            return helper();
        }

        fun helper(): i32 {
            return 42;
        }
    )";

    const char* b_source = R"(
        fun helper(x: i32, y: i32): i32 {
            return x + y;
        }

        pub fun sum(x: i32, y: i32): i32 {
            return helper(x, y);
        }
    )";

    const char* main_source = R"(
        from a import answer;
        from b import sum;

        fun main(): i32 {
            return answer() + sum(3, 4);
        }
    )";

    Compiler compiler(allocator);
    compiler.add_source("a", a_source, static_cast<u32>(strlen(a_source)));
    compiler.add_source("b", b_source, static_cast<u32>(strlen(b_source)));
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
    CHECK(result.as_int == 49);  // answer()=42 + sum(3,4)=7 = 49

    vm_destroy(&vm);
    delete module;
}

} // namespace rx

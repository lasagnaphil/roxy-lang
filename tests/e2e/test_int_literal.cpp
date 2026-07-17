#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Integer Literal Polymorphism Tests
// ============================================================================

TEST_SUITE("E2E Int Literals") {

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to i64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to i8", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun check(x: i8): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: i8 = 42;
            print(f"{check(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to u32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun check(x: u32): i32 {
            return i32(x);
        }

        fun main(): i32 {
            var x: u32 = 42;
            print(f"{check(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral basic coercion to u64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: u64 = 42;
            print(f"{i64(x)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral default inference is i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral function argument coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun add_one(x: i64): i64 {
            return x + 1l;
        }

        fun main(): i32 {
            print(f"{add_one(42)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral return statement coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun get_value(): i64 {
            return 42;
        }

        fun main(): i32 {
            print(f"{get_value()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral binary ops with suffixed literal", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: i64 = 42 + 1l;
            print(f"{a}");
            var b: i64 = 1l + 42;
            print(f"{b}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral binary ops default to i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x = 42 + 1;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "43\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral unary negate coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = -42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "-42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral compound assignment", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x += 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral ternary coercion compiles", Backend, RX_E2E_BACKENDS) {
        // Ternary with int literal coercion should compile (runtime value depends
        // on simplified ternary implementation that always takes the else branch)
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = false ? 0l : 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral non-literal still strict", Backend, RX_E2E_BACKENDS) {
        // A concrete i32 variable should NOT be assignable to i64
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var y: i64 = x;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("IntLiteral simple assignment coercion", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 0l;
            x = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral struct field init", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i64;
            y: i64;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            print(f"{p.x + p.y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral comparison with typed variable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 42l;
            if (x == 42) {
                print(f"{1}");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral non-integer target fails", Backend, RX_E2E_BACKENDS) {
        // Cannot assign integer literal to string
        const char* source = R"(
        fun main(): i32 {
            var x: bool = 42;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("IntLiteral suffixed literal still strict", Backend, RX_E2E_BACKENDS) {
        // 42l is i64, cannot assign to i32
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42l;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Equality works on all integer kinds", Backend, RX_E2E_BACKENDS) {
        // eq/ne used to be registered only for i32/i64; u8..u64 comparisons
        // silently typed as Error. Equality is a bit comparison on canonical
        // values, so it is now registered for every integer kind.
        const char* source = R"(
        fun main(): i32 {
            var a: u32 = 40;
            var b: u32 = 40;
            var c: u64 = 9000000000ul;
            var d: u64 = 9000000000ul;
            var r: i32 = 0;
            if (a == b) { r = r + 1; }
            if (c == d) { r = r + 2; }
            if (a != b) { r = r + 100; }
            return r;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Unsigned arithmetic is supported", Backend, RX_E2E_BACKENDS) {
        // u32 + u32 once typed as Error with no diagnostic; it now has native
        // unsigned arithmetic (see test_unsigned_arith.cpp for the semantics).
        const char* source = R"(
        fun main(): i32 {
            var x: u32 = 40;
            var y: u32 = 2;
            var z: u32 = x + y;
            print(f"{z}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    // ========================================================================
    // Adaptation to float contexts, and surviving constant folding.
    //
    // An unsuffixed literal is polymorphic until a context picks its type; it
    // adapts to any numeric type, float included, and an all-literal arithmetic
    // expression stays polymorphic so the context still gets to choose. Typed
    // values are unaffected and still never mix implicitly.
    //
    // The float cases check *values*, not just that they compile: the literal
    // carries an integer payload, so a float-typed literal has to be converted
    // rather than reinterpreted (1 read back as raw bits is 4.94e-324).
    // ========================================================================

    TEST_CASE_TEMPLATE("IntLiteral adapts to f64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: f64 = 1;
            print(f"{x}");
            print(f"{x + 1.5}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2.5\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral adapts to f32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: f32 = 2;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral mixed with a float operand", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: f64 = 1 + 2.0;
            var y: f64 = 7.0;
            print(f"{x}");
            print(f"{y / 2}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n3.5\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral survives folding into i64", Backend, RX_E2E_BACKENDS) {
        // `var x: i64 = 1;` and `1 + 2l` both worked; `1 + 2` did not, because
        // an all-literal binary defaulted to i32 before the annotation applied.
        const char* source = R"(
        fun main(): i32 {
            var x: i64 = 1 + 2;
            var y: i64 = (1 + 2) * 3;
            print(f"{x}");
            print(f"{y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n9\n");
    }

    TEST_CASE_TEMPLATE("IntLiteral adapts through a float parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun scale(v: f64): f64 {
            return v * 2.0;
        }

        fun main(): i32 {
            print(f"{scale(3)}");
            var l: List<f64> = List<f64>();
            l.push(4);
            print(f"{l[0]}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "6\n4\n");
    }

    TEST_CASE_TEMPLATE("Unannotated literal still defaults to i32", Backend, RX_E2E_BACKENDS) {
        // Polymorphism must not leak: with no context to choose a type, both a
        // bare literal and an all-literal expression settle on i32.
        const char* source = R"(
        fun main(): i32 {
            var x = 1;
            var y = 1 + 2;
            return x + y;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4);
    }

    TEST_CASE_TEMPLATE("Unsuffixed float literal adapts to f32", Backend, RX_E2E_BACKENDS) {
        // `1.0` is polymorphic like `1` is; `1.0f` is already concrete f32.
        const char* source = R"(
        fun main(): i32 {
            var a: f32 = 1.5;
            var b: f32 = 1.0 + 2.0f;
            var c: f64 = 1.0;
            print(f"{a}");
            print(f"{b}");
            print(f"{c}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1.5\n3\n1\n");
    }

    TEST_CASE_TEMPLATE("All-literal float expressions stay polymorphic", Backend, RX_E2E_BACKENDS) {
        // The arms/operands must follow the annotation; leaving them at the f64
        // default while the expression claims f32 reads back the wrong bits.
        const char* source = R"(
        fun main(): i32 {
            var a: f32 = 1.0 + 2.0;
            var b: f32 = true ? 1.5 : 2.5;
            var c: f64 = -2.5;
            print(f"{a}");
            print(f"{b}");
            print(f"{c}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n1.5\n-2.5\n");
    }

    TEST_CASE_TEMPLATE("Unannotated float literal still defaults to f64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x = 1.0;
            var y = 1.0 + 2.0;
            print(f"{x + y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "4\n");
    }

    TEST_CASE("Compile error: float literal doesn't adapt to an integer") {
        // Adaptation never introduces a truncating conversion.
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 1.0;
            return 0;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("Compile error: typed i32 and i64 still don't mix") {
        // Adaptation applies to literals only — typed values stay strict.
        const char* source = R"(
        fun main(): i32 {
            var a: i32 = 1;
            var b: i64 = 2l;
            var c: i64 = a + b;
            return 0;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

}  // TEST_SUITE("E2E Int Literals")

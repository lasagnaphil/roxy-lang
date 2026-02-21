#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Generic Function Tests
// ============================================================================

TEST_CASE("E2E - Generic function identity i32") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function identity f64") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: f64 = identity<f64>(3.14);
            return i32(x);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Generic function identity bool") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: bool = identity<bool>(true);
            if (x) { return 1; }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Generic function multiple instantiations") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var a: i32 = identity<i32>(10);
            var b: i32 = identity<i32>(20);
            var c: f64 = identity<f64>(1.5);
            return a + b + i32(c);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 31);
}

TEST_CASE("E2E - Generic function two type params") {
    const char* source = R"(
        fun first<T, U>(a: T, b: U): T {
            return a;
        }

        fun main(): i32 {
            return first<i32, f64>(42, 3.14);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function second of two params") {
    const char* source = R"(
        fun second<T, U>(a: T, b: U): U {
            return b;
        }

        fun main(): i32 {
            var x: f64 = second<i32, f64>(42, 2.5);
            return i32(x);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 2);
}

TEST_CASE("E2E - Generic function with computation") {
    const char* source = R"(
        fun double_val<T>(value: T): T {
            return value + value;
        }

        fun main(): i32 {
            return double_val<i32>(21);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function with struct type arg") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun get_x<T>(p: T): i32 {
            return 99;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            return get_x<Point>(p);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
}

// ============================================================================
// Generic Struct Tests
// ============================================================================

TEST_CASE("E2E - Generic struct literal i32") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic struct literal f64") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<f64> = Box<f64> { value = 3.14 };
            return i32(b.value);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Generic struct two type params") {
    const char* source = R"(
        struct Pair<T, U> {
            first: T;
            second: U;
        }

        fun main(): i32 {
            var p: Pair<i32, f64> = Pair<i32, f64> { first = 10, second = 2.5 };
            return p.first + i32(p.second);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 12);
}

TEST_CASE("E2E - Generic struct different instantiations") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var a: Box<i32> = Box<i32> { value = 10 };
            var b: Box<f64> = Box<f64> { value = 5.5 };
            return a.value + i32(b.value);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 15);
}

TEST_CASE("E2E - Generic struct as function parameter") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun unbox(b: Box<i32>): i32 {
            return b.value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return unbox(b);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic struct as function return type") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun make_box(v: i32): Box<i32> {
            return Box<i32> { value = v };
        }

        fun main(): i32 {
            var b: Box<i32> = make_box(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic function with generic struct") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun wrap<T>(v: T): Box<T> {
            return Box<T> { value = v };
        }

        fun main(): i32 {
            var b: Box<i32> = wrap<i32>(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

// ============================================================================
// Generic Type Argument Inference Tests
// ============================================================================

TEST_CASE("E2E - Generic inference: identity i32") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic inference: identity f64") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: f64 = identity(3.14);
            return i32(x);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Generic inference: two type params") {
    const char* source = R"(
        fun first<T, U>(a: T, b: U): T {
            return a;
        }

        fun main(): i32 {
            return first(42, 3.14);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic inference: function with struct arg") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun get_x<T>(p: T): i32 {
            return 99;
        }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 20 };
            return get_x(p);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
}

TEST_CASE("E2E - Generic inference: computation") {
    const char* source = R"(
        fun double_val<T>(value: T): T {
            return value + value;
        }

        fun main(): i32 {
            return double_val(21);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic inference: struct literal") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<i32> = Box { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic inference: struct literal multiple params") {
    const char* source = R"(
        struct Pair<T, U> {
            first: T;
            second: U;
        }

        fun main(): i32 {
            var p: Pair<i32, f64> = Pair { first = 10, second = 2.5 };
            return p.first + i32(p.second);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 12);
}

TEST_CASE("E2E - Generic inference: function returning generic struct") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun wrap<T>(v: T): Box<T> {
            return Box<T> { value = v };
        }

        fun main(): i32 {
            var b: Box<i32> = wrap(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic inference: backward compat explicit args") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

// ============================================================================
// Variable Type Inference with Generic RHS
// ============================================================================

TEST_CASE("E2E - Generic var inference: struct literal inferred") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b = Box { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic var inference: function call inferred") {
    const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var b = identity(42);
            return b;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic var inference: function returning generic struct") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun wrap<T>(v: T): Box<T> {
            return Box<T> { value = v };
        }

        fun main(): i32 {
            var b = wrap(42);
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic var inference: multiple type params") {
    const char* source = R"(
        struct Pair<T, U> {
            first: T;
            second: U;
        }

        fun main(): i32 {
            var p = Pair { first = 10, second = 2.5 };
            return p.first + i32(p.second);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 12);
}

TEST_CASE("E2E - Generic var inference: explicit type args no LHS annotation") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b = Box<i32> { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic var inference: chained field access") {
    const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b = Box { value = 42 };
            var v = b.value;
            return v;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

// ============================================================================
// Generic Compound Type Argument Tests (name mangling)
// ============================================================================

TEST_CASE("E2E - Generic function with List type argument") {
    const char* source = R"(
        fun get_len<T>(lst: T): i32 {
            return 0;
        }

        fun main(): i32 {
            var a: List<i32> = List<i32>();
            a.push(10);
            var b: List<f32> = List<f32>();
            b.push(1.5f);
            return get_len<List<i32>>(a) + get_len<List<f32>>(b) + a.len();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Generic struct with List type argument") {
    const char* source = R"(
        struct Wrapper<T> {
            value: T;
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(42);
            var w = Wrapper<List<i32>> { value = lst };
            return w.value.len();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Generic inference: error uninferrable param") {
    const char* source = R"(
        fun make_default<T>(): T {
            var x: T;
            return x;
        }

        fun main(): i32 {
            return make_default();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

// ============================================================================
// Trait Bound Tests (Bounded Quantification - Phase A)
// Phase A: checks bounds at instantiation sites only (not in generic bodies)
// ============================================================================

TEST_CASE("E2E - Generic bound: single bound Printable") {
    const char* source = R"(
        fun identity_printable<T: Printable>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_printable<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: Hash bound") {
    const char* source = R"(
        fun identity_hash<T: Hash>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_hash<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: multiple bounds Printable + Hash") {
    const char* source = R"(
        fun identity_both<T: Printable + Hash>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_both<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: bound on generic struct") {
    const char* source = R"(
        struct HashBox<T: Hash> {
            value: T;
        }

        fun main(): i32 {
            var b: HashBox<i32> = HashBox<i32> { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: inferred type args with bound") {
    const char* source = R"(
        fun identity_printable<T: Printable>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_printable(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: inferred struct literal with bound") {
    const char* source = R"(
        struct HashBox<T: Hash> {
            value: T;
        }

        fun main(): i32 {
            var b: HashBox<i32> = HashBox { value = 42 };
            return b.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Generic bound: bound not satisfied (negative)") {
    const char* source = R"(
        trait Flyable;

        fun require_fly<T: Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return require_fly<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - Generic bound: generic trait bound with type args") {
    const char* source = R"(
        trait Scalable<T>;

        fun Scalable.scale(factor: T): Self;

        struct Vec2 {
            x: i32;
            y: i32;
        }

        fun Vec2.scale(factor: Vec2): Vec2 for Scalable<Vec2> {
            return Vec2 { x = self.x + factor.x, y = self.y + factor.y };
        }

        fun apply_scale<T: Scalable<Vec2>>(v: T): i32 {
            return 1;
        }

        fun main(): i32 {
            var v: Vec2 = Vec2 { x = 1, y = 2 };
            return apply_scale<Vec2>(v);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Generic bound: generic trait bound mismatch (negative)") {
    const char* source = R"(
        trait Scalable<T>;

        fun Scalable.scale(factor: T): Self;

        struct Vec2 {
            x: i32;
            y: i32;
        }

        fun Vec2.scale(factor: Vec2): Vec2 for Scalable<Vec2> {
            return Vec2 { x = self.x + factor.x, y = self.y + factor.y };
        }

        fun apply_scale<T: Scalable<i32>>(v: T): i32 {
            return 1;
        }

        fun main(): i32 {
            var v: Vec2 = Vec2 { x = 1, y = 2 };
            return apply_scale<Vec2>(v);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - Generic bound: one of multiple bounds not satisfied (negative)") {
    const char* source = R"(
        trait Flyable;

        fun needs_both<T: Printable + Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return needs_both<i32>(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - Generic bound: bound not satisfied on struct (negative)") {
    const char* source = R"(
        trait Flyable;

        struct FlyBox<T: Flyable> {
            value: T;
        }

        fun main(): i32 {
            var b: FlyBox<i32> = FlyBox<i32> { value = 42 };
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - Generic bound: inferred type args violate bound (negative)") {
    const char* source = R"(
        trait Flyable;

        fun require_fly<T: Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return require_fly(42);
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

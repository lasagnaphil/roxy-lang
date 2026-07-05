#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Generic Function Tests
// ============================================================================

TEST_SUITE("E2E Generics") {

    TEST_CASE_TEMPLATE("Generic function identity i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic function identity f64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: f64 = identity<f64>(3.14);
            return i32(x);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Generic function identity bool", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Generic function multiple instantiations", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 31);
    }

    TEST_CASE_TEMPLATE("Generic function two type params", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun first<T, U>(a: T, b: U): T {
            return a;
        }

        fun main(): i32 {
            return first<i32, f64>(42, 3.14);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic function second of two params", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun second<T, U>(a: T, b: U): U {
            return b;
        }

        fun main(): i32 {
            var x: f64 = second<i32, f64>(42, 2.5);
            return i32(x);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

    TEST_CASE_TEMPLATE("Generic function with computation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun double_val<T>(value: T): T {
            return value + value;
        }

        fun main(): i32 {
            return double_val<i32>(21);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic function with struct type arg", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    // ============================================================================
    // Generic Struct Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic struct literal i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct literal f64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<f64> = Box<f64> { value = 3.14 };
            return i32(b.value);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Generic struct two type params", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 12);
    }

    TEST_CASE_TEMPLATE("Generic struct different instantiations", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 15);
    }

    TEST_CASE_TEMPLATE("Generic struct as function parameter", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct as function return type", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic function with generic struct", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Generic Type Argument Inference Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic inference: identity i32", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic inference: identity f64", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var x: f64 = identity(3.14);
            return i32(x);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Generic inference: two type params", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun first<T, U>(a: T, b: U): T {
            return a;
        }

        fun main(): i32 {
            return first(42, 3.14);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic inference: function with struct arg", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    TEST_CASE_TEMPLATE("Generic inference: computation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun double_val<T>(value: T): T {
            return value + value;
        }

        fun main(): i32 {
            return double_val(21);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic inference: struct literal", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b: Box<i32> = Box { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic inference: struct literal multiple params", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 12);
    }

    TEST_CASE_TEMPLATE("Generic inference: function returning generic struct", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic inference: backward compat explicit args", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Variable Type Inference with Generic RHS
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic var inference: struct literal inferred", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b = Box { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic var inference: function call inferred", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            var b = identity(42);
            return b;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic var inference: function returning generic struct", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic var inference: multiple type params", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 12);
    }

    TEST_CASE_TEMPLATE("Generic var inference: explicit type args no LHS annotation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun main(): i32 {
            var b = Box<i32> { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic var inference: chained field access", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Generic Compound Type Argument Tests (name mangling)
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic function with List type argument", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun get_len<T>(lst: T): i32 {
            return 0;
        }

        fun main(): i32 {
            var a: List<i32> = List<i32>();
            a.push(10);
            var b: List<f32> = List<f32>();
            b.push(1.5f);
            var n: i32 = a.len();   // containers are move-only — read len before passing a by value
            return get_len<List<i32>>(a) + get_len<List<f32>>(b) + n;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Generic struct with List type argument", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Generic inference: error uninferrable param", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun make_default<T>(): T {
            var x: T;
            return x;
        }

        fun main(): i32 {
            return make_default();
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    // ============================================================================
    // Trait Bound Tests (Bounded Quantification - Phase A)
    // Phase A: checks bounds at instantiation sites only (not in generic bodies)
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic bound: single bound Printable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity_printable<T: Printable>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_printable<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: Hash bound", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity_hash<T: Hash>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_hash<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: multiple bounds Printable + Hash", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity_both<T: Printable + Hash>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_both<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: bound on generic struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct HashBox<T: Hash> {
            value: T;
        }

        fun main(): i32 {
            var b: HashBox<i32> = HashBox<i32> { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: inferred type args with bound", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity_printable<T: Printable>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_printable(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: inferred struct literal with bound", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct HashBox<T: Hash> {
            value: T;
        }

        fun main(): i32 {
            var b: HashBox<i32> = HashBox { value = 42 };
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic bound: bound not satisfied (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Flyable;

        fun require_fly<T: Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return require_fly<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic bound: generic trait bound with type args", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Generic bound: generic trait bound mismatch (negative)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic bound: one of multiple bounds not satisfied (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Flyable;

        fun needs_both<T: Printable + Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return needs_both<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic bound: bound not satisfied on struct (negative)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic bound: inferred type args violate bound (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Flyable;

        fun require_fly<T: Flyable>(value: T): i32 {
            return 0;
        }

        fun main(): i32 {
            return require_fly(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    // ============================================================================
    // Phase B: Definition-site Trait Bound Checking Tests
    // Validates that generic template bodies are checked against declared bounds
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic Phase B: call trait method on bounded param", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Greetable;
        fun Greetable.greet(): i32;

        struct Person { id: i32; }
        fun Person.greet(): i32 for Greetable { return self.id; }

        fun call_greet<T: Greetable>(v: T): i32 { return v.greet(); }

        fun main(): i32 {
            var p: Person = Person { id = 42 };
            return call_greet<Person>(p);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: call methods from multiple bounds", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait HasX;
        fun HasX.get_x(): i32;

        trait HasY;
        fun HasY.get_y(): i32;

        struct Point { x: i32; y: i32; }
        fun Point.get_x(): i32 for HasX { return self.x; }
        fun Point.get_y(): i32 for HasY { return self.y; }

        fun sum_xy<T: HasX + HasY>(v: T): i32 { return v.get_x() + v.get_y(); }

        fun main(): i32 {
            var p: Point = Point { x = 10, y = 32 };
            return sum_xy<Point>(p);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: return type Self resolves to T", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Addable;
        fun Addable.add(other: Self): Self;

        struct Num { val: i32; }
        fun Num.add(other: Num): Num for Addable {
            return Num { val = self.val + other.val };
        }

        fun double_add<T: Addable>(a: T, b: T): T { return a.add(b); }

        fun main(): i32 {
            var a: Num = Num { val = 10 };
            var b: Num = Num { val = 32 };
            var c: Num = double_add<Num>(a, b);
            return c.val;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: generic trait bound with type args", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Scalable<T>;
        fun Scalable.scale(factor: T): Self;

        struct Vec2 { x: i32; y: i32; }
        fun Vec2.scale(factor: Vec2): Vec2 for Scalable<Vec2> {
            return Vec2 { x = self.x + factor.x, y = self.y + factor.y };
        }

        fun apply_scale<T: Scalable<Vec2>>(v: T, f: Vec2): T {
            return v.scale(f);
        }

        fun main(): i32 {
            var v: Vec2 = Vec2 { x = 10, y = 20 };
            var f: Vec2 = Vec2 { x = 5, y = 7 };
            var r: Vec2 = apply_scale<Vec2>(v, f);
            return r.x + r.y;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: bounded template with passthrough return", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun identity_bounded<T: Printable>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity_bounded<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Phase B Negative Tests: definition-site errors
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic Phase B: call method not in any bound (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Greetable;
        fun Greetable.greet(): i32;

        fun bad<T: Greetable>(v: T): i32 { return v.nonexistent(); }

        fun main(): i32 { return 0; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: call method from wrong trait (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Greetable;
        fun Greetable.greet(): i32;

        fun bad<T: Greetable>(v: T): i32 { return v.hash(); }

        fun main(): i32 { return 0; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: access field on type param (negative)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        trait Greetable;
        fun Greetable.greet(): i32;

        fun bad<T: Greetable>(v: T): i32 { return v.x; }

        fun main(): i32 { return 0; }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Generic Phase B: unbounded template body not checked", Backend, RX_E2E_BACKENDS) {
        // Unbounded generic templates are NOT checked at definition site
        // (only checked at instantiation). This should compile fine as long
        // as main doesn't instantiate bad_func.
        const char* source = R"(
        fun identity<T>(value: T): T {
            return value;
        }

        fun main(): i32 {
            return identity<i32>(42);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Generic Struct Methods Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic struct method: basic external getter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun Box<T>.get(): T {
            return self.value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 42 };
            return b.get();
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct method: setter and getter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun Box<T>.get(): T {
            return self.value;
        }

        fun Box<T>.set(v: T) {
            self.value = v;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 0 };
            b.set(42);
            return b.get();
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct method: multiple instantiations", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Wrapper<T> {
            val: T;
        }

        fun Wrapper<T>.unwrap(): T {
            return self.val;
        }

        fun main(): i32 {
            var a: Wrapper<i32> = Wrapper<i32> { val = 10 };
            var b: Wrapper<f64> = Wrapper<f64> { val = 5.5 };
            return a.unwrap() + i32(b.unwrap());
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 15);
    }

    TEST_CASE_TEMPLATE("Generic struct method: cross-method calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Counter<T> {
            value: T;
            count: i32;
        }

        fun Counter<T>.get_count(): i32 {
            return self.count;
        }

        fun Counter<T>.is_empty(): bool {
            return self.get_count() == 0;
        }

        fun main(): i32 {
            var c: Counter<i32> = Counter<i32> { value = 99, count = 0 };
            if (c.is_empty()) {
                return 42;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct method: return struct type", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Pair<T> {
            first: T;
            second: T;
        }

        fun Pair<T>.swap(): Pair<T> {
            return Pair<T> { first = self.second, second = self.first };
        }

        fun main(): i32 {
            var p: Pair<i32> = Pair<i32> { first = 10, second = 32 };
            var q: Pair<i32> = p.swap();
            return q.first + q.second;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct method: with additional parameters", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun Box<T>.add(other: T): T {
            return self.value + other;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32> { value = 10 };
            return b.add(32);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Generic Struct Constructor/Destructor Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Generic struct: user-defined default constructor", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun new Box<T>(value: T) {
            self.value = value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32>(42);
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct: named constructor", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Pair<T> {
            first: T;
            second: T;
        }

        fun new Pair<T>.make(a: T, b: T) {
            self.first = a;
            self.second = b;
        }

        fun main(): i32 {
            var p: Pair<i32> = Pair<i32>.make(10, 32);
            return p.first + p.second;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct: constructor with method", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun new Box<T>(value: T) {
            self.value = value;
        }

        fun Box<T>.get(): T {
            return self.value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32>(42);
            return b.get();
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct: constructor multiple instantiations", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun new Box<T>(value: T) {
            self.value = value;
        }

        fun main(): i32 {
            var b1: Box<i32> = Box<i32>(40);
            var b2: Box<f64> = Box<f64>(2.5);
            return b1.value + i32(b2.value);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic struct: destructor with uniq", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun new Box<T>(value: T) {
            self.value = value;
        }

        fun delete Box<T>() {
            print(f"{self.value}");
        }

        fun main(): i32 {
            var b: uniq Box<i32> = uniq Box<i32>(99);
            delete b;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "99\n");
    }

    TEST_CASE_TEMPLATE("Generic struct: constructor suppresses synthesized default", Backend, RX_E2E_BACKENDS) {
        // User-defined default constructor should be used instead of synthesized one
        const char* source = R"(
        struct Box<T> {
            value: T;
        }

        fun new Box<T>(value: T) {
            self.value = value + value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box<i32>(21);
            return b.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Generic higher-order with closure", Backend, RX_E2E_BACKENDS) {
        // Generic function taking a fun(T) -> T parameter, called with a closure
        // that's stored in a local variable. Exercises three issues that all had to
        // line up: (1) substitute_type_expr must recurse into Function-kind
        // return_type so `fun(T) -> T` becomes `fun(i32) -> i32`; (2) the generic
        // call site must consume noncopyable args (closure values are
        // noncopyable); (3) analyze_generic_fun_call must set
        // ce.callee->resolved_type so the IR builder emits Nullify (not Delete) on
        // the moved closure local at scope exit.
        SUBCASE("Explicit type args") {
            const char* source = R"(
            fun apply<T>(f: fun(T) -> T, x: T): T {
                return f(x);
            }
            fun main(): i32 {
                var double = fun(x: i32): i32 => x * 2;
                return apply<i32>(double, 21);
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.value == 42);
        }

        SUBCASE("Inferred type args") {
            const char* source = R"(
            fun apply<T>(f: fun(T) -> T, x: T): T {
                return f(x);
            }
            fun main(): i32 {
                var double = fun(x: i32): i32 => x * 2;
                return apply(double, 21);
            }
        )";
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.value == 42);
        }
    }

    TEST_CASE_TEMPLATE("Generic inference from Map parameter", Backend, RX_E2E_BACKENDS) {
        // unify_type_expr used to special-case only List among the builtin
        // containers, so K/V could not be inferred from a Map argument.
        const char* source = R"(
        fun map_len<K, V>(m: inout Map<K, V>): i32 {
            return m.len();
        }

        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            m.insert("a", 1);
            m.insert("b", 2);
            return map_len(inout m);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

}  // TEST_SUITE("E2E Generics")

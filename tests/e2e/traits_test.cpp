#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Trait Tests
// ============================================================================

TEST_CASE("E2E - Trait basic required method") {
    const char* source = R"(
        trait Describable;

        fun Describable.value(): i32;

        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.value(): i32 for Describable {
            return self.x + self.y;
        }

        fun main(): i32 {
            var p: Point = Point { x = 3, y = 7 };
            print(p.value());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n");
}

TEST_CASE("E2E - Trait default method") {
    const char* source = R"(
        trait Summable;

        fun Summable.sum(): i32;

        fun Summable.double_sum(): i32 {
            return self.sum() * 2;
        }

        struct Pair {
            a: i32;
            b: i32;
        }

        fun Pair.sum(): i32 for Summable {
            return self.a + self.b;
        }

        fun main(): i32 {
            var p: Pair = Pair { a = 5, b = 3 };
            print(p.sum());
            print(p.double_sum());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "8\n16\n");
}

TEST_CASE("E2E - Trait inheritance") {
    const char* source = R"(
        trait Base;
        fun Base.base_val(): i32;

        trait Child : Base;
        fun Child.child_val(): i32;

        struct Foo {
            x: i32;
        }

        fun Foo.base_val(): i32 for Base {
            return self.x;
        }

        fun Foo.child_val(): i32 for Child {
            return self.x * 2;
        }

        fun main(): i32 {
            var f: Foo = Foo { x = 5 };
            print(f.base_val());
            print(f.child_val());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n10\n");
}

TEST_CASE("E2E - Trait missing required method") {
    const char* source = R"(
        trait Describable;
        fun Describable.value(): i32;
        fun Describable.name(): i32;

        struct Point {
            x: i32;
        }

        fun Point.value(): i32 for Describable {
            return self.x;
        }

        fun main(): i32 {
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Trait Eq operator") {
    const char* source = R"(
        trait Eq;
        fun Eq.eq(other: Self): bool;
        fun Eq.ne(other: Self): bool {
            if (self.eq(other)) return false;
            return true;
        }

        struct Vec2 {
            x: i32;
            y: i32;
        }

        fun Vec2.eq(other: Vec2): bool for Eq {
            return self.x == other.x && self.y == other.y;
        }

        fun bool_to_int(b: bool): i32 {
            if (b) return 1;
            return 0;
        }

        fun main(): i32 {
            var a: Vec2 = Vec2 { x = 1, y = 2 };
            var b: Vec2 = Vec2 { x = 1, y = 2 };
            var c: Vec2 = Vec2 { x = 3, y = 4 };
            print(bool_to_int(a == b));
            print(bool_to_int(a == c));
            print(bool_to_int(a != c));
            print(bool_to_int(a != b));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n0\n1\n0\n");
}

TEST_CASE("E2E - Trait Ord operator with inheritance") {
    const char* source = R"(
        trait Eq;
        fun Eq.eq(other: Self): bool;

        trait Ord : Eq;
        fun Ord.lt(other: Self): bool;

        fun Ord.le(other: Self): bool {
            return self.lt(other) || self.eq(other);
        }
        fun Ord.gt(other: Self): bool {
            if (self.lt(other)) return false;
            if (self.eq(other)) return false;
            return true;
        }
        fun Ord.ge(other: Self): bool {
            if (self.lt(other)) return false;
            return true;
        }

        struct Score {
            value: i32;
        }

        fun Score.eq(other: Score): bool for Eq {
            return self.value == other.value;
        }

        fun Score.lt(other: Score): bool for Ord {
            return self.value < other.value;
        }

        fun bool_to_int(b: bool): i32 {
            if (b) return 1;
            return 0;
        }

        fun main(): i32 {
            var a: Score = Score { value = 10 };
            var b: Score = Score { value = 20 };
            var c: Score = Score { value = 10 };
            print(bool_to_int(a < b));
            print(bool_to_int(b < a));
            print(bool_to_int(a <= c));
            print(bool_to_int(a > b));
            print(bool_to_int(b > a));
            print(bool_to_int(a >= c));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n0\n1\n0\n1\n1\n");
}

TEST_CASE("E2E - Trait override default method") {
    const char* source = R"(
        trait Summable;

        fun Summable.sum(): i32;

        fun Summable.double_sum(): i32 {
            return self.sum() * 2;
        }

        struct Pair {
            a: i32;
            b: i32;
        }

        fun Pair.sum(): i32 for Summable {
            return self.a + self.b;
        }

        fun Pair.double_sum(): i32 for Summable {
            return (self.a + self.b) * 3;
        }

        fun main(): i32 {
            var p: Pair = Pair { a = 5, b = 3 };
            print(p.sum());
            print(p.double_sum());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "8\n24\n");
}

TEST_CASE("E2E - Trait Self type in parameters") {
    const char* source = R"(
        trait Addable;

        fun Addable.add(other: Self): Self;

        struct Num {
            val: i32;
        }

        fun Num.add(other: Num): Num for Addable {
            return Num { val = self.val + other.val };
        }

        fun main(): i32 {
            var a: Num = Num { val = 10 };
            var b: Num = Num { val = 25 };
            var c: Num = a.add(b);
            print(c.val);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "35\n");
}

TEST_CASE("E2E - Multiple traits on one struct") {
    const char* source = R"(
        trait HasX;
        fun HasX.get_x(): i32;

        trait HasY;
        fun HasY.get_y(): i32;

        struct Point {
            x: i32;
            y: i32;
        }

        fun Point.get_x(): i32 for HasX {
            return self.x;
        }

        fun Point.get_y(): i32 for HasY {
            return self.y;
        }

        fun main(): i32 {
            var p: Point = Point { x = 42, y = 99 };
            print(p.get_x());
            print(p.get_y());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n99\n");
}

TEST_CASE("E2E - Struct method takes priority over trait default") {
    const char* source = R"(
        trait Greetable;
        fun Greetable.greet(): i32 {
            return 0;
        }

        struct Person {
            id: i32;
        }

        fun Person.greet(): i32 {
            return self.id;
        }

        fun main(): i32 {
            var p: Person = Person { id = 42 };
            print(p.greet());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Trait method not in trait error") {
    const char* source = R"(
        trait Foo;
        fun Foo.bar(): i32;

        struct Baz {
            x: i32;
        }

        fun Baz.bar(): i32 for Foo {
            return self.x;
        }

        fun Baz.qux(): i32 for Foo {
            return self.x * 2;
        }

        fun main(): i32 {
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Stack-Allocated Constructor Tests (Type())
// ============================================================================

TEST_CASE("E2E - Constructor: stack-allocated default constructor") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point() {
            self.x = 10;
            self.y = 20;
        }

        fun main(): i32 {
            var p: Point = Point();
            print(f"{p.x}");
            print(f"{p.y}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Constructor: stack-allocated with params") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point(x: i32, y: i32) {
            self.x = x;
            self.y = y;
        }

        fun main(): i32 {
            var p: Point = Point(5, 15);
            print(f"{p.x}");
            print(f"{p.y}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n15\n");
}

TEST_CASE("E2E - Constructor: stack-allocated named constructor") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point.from_coords(x: i32, y: i32) {
            self.x = x;
            self.y = y;
        }

        fun main(): i32 {
            var p: Point = Point.from_coords(7, 14);
            print(f"{p.x}");
            print(f"{p.y}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "7\n14\n");
}

// ============================================================================
// Heap-Allocated Constructor Tests (uniq Type())
// ============================================================================

TEST_CASE("E2E - Constructor: heap-allocated default constructor") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point() {
            self.x = 10;
            self.y = 20;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            print(f"{p.x}");
            print(f"{p.y}");
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Constructor: heap-allocated with params") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point(x: i32, y: i32) {
            self.x = x;
            self.y = y;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point(5, 15);
            print(f"{p.x}");
            print(f"{p.y}");
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n15\n");
}

TEST_CASE("E2E - Constructor: heap-allocated named constructor") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point.from_coords(x: i32, y: i32) {
            self.x = x;
            self.y = y;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point.from_coords(7, 14);
            print(f"{p.x}");
            print(f"{p.y}");
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "7\n14\n");
}

TEST_CASE("E2E - Constructor: multiple constructors") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun new Point() {
            self.x = 0;
            self.y = 0;
        }

        fun new Point.from_coords(x: i32, y: i32) {
            self.x = x;
            self.y = y;
        }

        fun new Point.from_value(val: i32) {
            self.x = val;
            self.y = val;
        }

        fun main(): i32 {
            var p1: uniq Point = uniq Point();
            var p2: uniq Point = uniq Point.from_coords(3, 6);
            var p3: uniq Point = uniq Point.from_value(9);

            print(f"{p1.x}");
            print(f"{p1.y}");
            print(f"{p2.x}");
            print(f"{p2.y}");
            print(f"{p3.x}");
            print(f"{p3.y}");

            delete p1;
            delete p2;
            delete p3;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n0\n3\n6\n9\n9\n");
}

// ============================================================================
// Destructor Tests
// ============================================================================

TEST_CASE("E2E - Destructor: default destructor") {
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun new Counter(v: i32) {
            self.value = v;
        }

        fun delete Counter() {
            print(f"{100}");
        }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(42);
            print(f"{c.value}");
            delete c;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n100\n");
}

TEST_CASE("E2E - Destructor: named destructor with args") {
    const char* source = R"(
        struct Data {
            value: i32;
        }

        fun new Data(v: i32) {
            self.value = v;
        }

        fun delete Data.save(multiplier: i32) {
            print(f"{self.value * multiplier}");
        }

        fun main(): i32 {
            var d: uniq Data = uniq Data(10);
            delete d.save(5);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "50\n");  // 10 * 5
}

TEST_CASE("E2E - Destructor: multiple destructors") {
    const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun new Resource(id: i32) {
            self.id = id;
        }

        fun delete Resource() {
            print(f"{self.id}");
        }

        fun delete Resource.with_message(msg: i32) {
            print(f"{msg}");
            print(f"{self.id}");
        }

        fun main(): i32 {
            var r1: uniq Resource = uniq Resource(1);
            var r2: uniq Resource = uniq Resource(2);

            delete r2.with_message(999);
            delete r1;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "999\n2\n1\n");
}

// ============================================================================
// Constructor and Destructor Together
// ============================================================================

TEST_CASE("E2E - Constructor + Destructor lifecycle") {
    const char* source = R"(
        struct Object {
            id: i32;
        }

        fun new Object(id: i32) {
            self.id = id;
            print(f"{1}");
        }

        fun delete Object() {
            print(f"{2}");
        }

        fun main(): i32 {
            print(f"{0}");
            var obj: uniq Object = uniq Object(42);
            print(f"{obj.id}");
            delete obj;
            print(f"{3}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n1\n42\n2\n3\n");
}

// ============================================================================
// No Constructor Tests (regression)
// ============================================================================

TEST_CASE("E2E - Constructor: no constructor defined (zero-init heap)") {
    const char* source = R"(
        struct Simple {
            value: i32;
        }

        fun main(): i32 {
            var s: uniq Simple = uniq Simple();
            print(f"{s.value}");
            delete s;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n");  // zero-initialized
}

TEST_CASE("E2E - Constructor: no constructor defined (zero-init stack)") {
    const char* source = R"(
        struct Simple {
            value: i32;
        }

        fun main(): i32 {
            var s: Simple = Simple();
            print(f"{s.value}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n");  // zero-initialized
}

// ============================================================================
// Public/Private Constructors
// ============================================================================

TEST_CASE("E2E - Constructor: pub constructor") {
    const char* source = R"(
        struct Widget {
            id: i32;
        }

        pub fun new Widget(id: i32) {
            self.id = id;
        }

        fun main(): i32 {
            var w: uniq Widget = uniq Widget(123);
            print(f"{w.id}");
            delete w;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "123\n");
}

// ============================================================================
// Struct literal syntax
// ============================================================================

TEST_CASE("E2E - Struct literal (stack)") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: Point = Point { x = 42, y = 99 };
            print(f"{p.x}");
            print(f"{p.y}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n99\n");
}

TEST_CASE("E2E - Struct literal (heap)") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point { x = 42, y = 99 };
            print(f"{p.x}");
            print(f"{p.y}");
            delete p;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n99\n");
}

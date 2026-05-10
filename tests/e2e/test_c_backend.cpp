#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

TEST_CASE("E2E - C Backend: Return constant") {
    const char* source = R"(
        fun main(): i32 {
            return 42;
        }
    )";

    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Integer arithmetic") {
    SUBCASE("Addition") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 10;
                var b: i32 = 20;
                return a + b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 30);
    }

    SUBCASE("Subtraction") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 50;
                var b: i32 = 8;
                return a - b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Multiplication") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 6;
                var b: i32 = 7;
                return a * b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Division") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 84;
                var b: i32 = 2;
                return a / b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Modulo") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 47;
                var b: i32 = 5;
                return a % b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
    }
}

TEST_CASE("E2E - C Backend: Negation") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = -42;
            return -a;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Comparisons and boolean logic") {
    SUBCASE("Less than true") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 5;
                var b: i32 = 10;
                if (a < b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Greater than false") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 5;
                var b: i32 = 10;
                if (a > b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0);
    }

    SUBCASE("Equality") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 42;
                var b: i32 = 42;
                if (a == b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Boolean AND") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = true;
                var b: bool = false;
                if (a && b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0);
    }

    SUBCASE("Boolean OR") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = true;
                var b: bool = false;
                if (a || b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Boolean NOT") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = false;
                if (!a) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }
}

TEST_CASE("E2E - C Backend: If/else control flow") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            if (x > 5) {
                return 1;
            } else {
                return 0;
            }
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 1);
}

TEST_CASE("E2E - C Backend: While loop") {
    const char* source = R"(
        fun main(): i32 {
            var sum: i32 = 0;
            var i: i32 = 1;
            while (i <= 10) {
                sum = sum + i;
                i = i + 1;
            }
            return sum;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 55);
}

TEST_CASE("E2E - C Backend: Simple function call") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
        fun main(): i32 {
            return add(20, 22);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Nested function calls") {
    const char* source = R"(
        fun double_val(x: i32): i32 {
            return x * 2;
        }
        fun add_one(x: i32): i32 {
            return x + 1;
        }
        fun main(): i32 {
            return add_one(double_val(20));
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 41);
}

TEST_CASE("E2E - C Backend: Recursive function") {
    const char* source = R"(
        fun factorial(n: i32): i32 {
            if (n <= 1) { return 1; }
            return n * factorial(n - 1);
        }
        fun main(): i32 {
            return factorial(5);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 120);
}

TEST_CASE("E2E - C Backend: Bitwise operations") {
    SUBCASE("AND") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xFF;
                var b: i32 = 0x0F;
                return a & b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0x0F);
    }

    SUBCASE("OR") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xF0;
                var b: i32 = 0x0F;
                return a | b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0xFF);
    }

    SUBCASE("XOR") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xFF;
                var b: i32 = 0xF0;
                return a ^ b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0x0F);
    }

    SUBCASE("Shift left") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 1;
                return a << 4;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 16);
    }

    SUBCASE("Shift right") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 64;
                return a >> 2;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 16);
    }
}

TEST_CASE("E2E - C Backend: Multiple functions") {
    const char* source = R"(
        fun min_val(a: i32, b: i32): i32 {
            if (a < b) { return a; }
            return b;
        }
        fun max_val(a: i32, b: i32): i32 {
            if (a > b) { return a; }
            return b;
        }
        fun clamp(x: i32, lo: i32, hi: i32): i32 {
            return min_val(max_val(x, lo), hi);
        }
        fun main(): i32 {
            return clamp(100, 0, 42);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

// ===== Phase 2: Structs, Enums, and Pointer Operations =====

TEST_CASE("E2E - C Backend: Struct basic") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun main(): i32 {
            var p: Point;
            p.x = 10;
            p.y = 32;
            return p.x + p.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct as parameter") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun sum_point(p: Point): i32 {
            return p.x + p.y;
        }
        fun main(): i32 {
            var p: Point;
            p.x = 20;
            p.y = 22;
            return sum_point(p);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct as return value") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun make_point(x: i32, y: i32): Point {
            var p: Point;
            p.x = x;
            p.y = y;
            return p;
        }
        fun main(): i32 {
            var p: Point = make_point(20, 22);
            return p.x + p.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct method") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun Point.sum(): i32 {
            return self.x + self.y;
        }
        fun main(): i32 {
            var p: Point;
            p.x = 20;
            p.y = 22;
            return p.sum();
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct constructor") {
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
            var p: Point = Point(20, 22);
            return p.x + p.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct inheritance") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }
        struct Dog : Animal {
            breed: i32;
        }
        fun main(): i32 {
            var d: Dog;
            d.hp = 30;
            d.breed = 12;
            return d.hp + d.breed;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct copy") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun main(): i32 {
            var p1: Point;
            p1.x = 20;
            p1.y = 22;
            var p2: Point = p1;
            return p2.x + p2.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Nested struct") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        struct Rect {
            origin: Point;
            size: Point;
        }
        fun main(): i32 {
            var r: Rect;
            r.origin.x = 10;
            r.origin.y = 20;
            r.size.x = 5;
            r.size.y = 7;
            return r.origin.x + r.origin.y + r.size.x + r.size.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Enum definition") {
    const char* source = R"(
        enum Color { Red, Green, Blue }
        fun main(): i32 {
            var c: Color = Color::Green;
            if (c == Color::Green) { return 42; }
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Struct with enum field") {
    const char* source = R"(
        enum Direction { North, East, South, West }
        struct Player {
            x: i32;
            y: i32;
            facing: Direction;
        }
        fun main(): i32 {
            var p: Player;
            p.x = 10;
            p.y = 20;
            p.facing = Direction::South;
            if (p.facing == Direction::South) {
                return p.x + p.y + 12;
            }
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Out parameter") {
    const char* source = R"(
        fun init_value(x: out i32) {
            x = 42;
        }
        fun main(): i32 {
            var n: i32 = 0;
            init_value(out n);
            return n;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Inout parameter") {
    const char* source = R"(
        fun increment(x: inout i32) {
            x = x + 1;
        }
        fun main(): i32 {
            var n: i32 = 41;
            increment(inout n);
            return n;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Large struct return") {
    const char* source = R"(
        struct BigData {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
            e: i32;
        }
        fun make_big(x: i32): BigData {
            var d: BigData;
            d.a = x;
            d.b = x + 1;
            d.c = x + 2;
            d.d = x + 3;
            d.e = x + 4;
            return d;
        }
        fun main(): i32 {
            var data: BigData = make_big(6);
            return data.a + data.b + data.c + data.d + data.e;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 40);
}

TEST_CASE("E2E - C Backend: Cast f64 to i32") {
    const char* source = R"(
        fun main(): i32 {
            var x: f64 = 42.7;
            var n: i32 = i32(x);
            return n;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Tagged union") {
    const char* source = R"(
        enum Kind { A, B }
        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }
        fun main(): i32 {
            var d: Data;
            d.kind = Kind::A;
            d.val_a = 42;
            return d.val_a;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: compile_to_cpp produces valid output") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
        fun main(): i32 {
            return add(1, 2);
        }
    )";

    String cpp_source = compile_to_cpp(source);
    CHECK(!cpp_source.empty());
    CHECK(cpp_source.find("#include <stdint.h>") != String::npos);
    CHECK(cpp_source.find("int32_t") != String::npos);
    CHECK(cpp_source.find("return") != String::npos);
}

// ===== Phase 3: Runtime Library =====

// --- Step 1: ConstString + CallNative(print) ---

TEST_CASE("E2E - C Backend: Print string literal") {
    const char* source = R"(
        fun main(): i32 {
            print("hello world");
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - C Backend: Multiple prints") {
    const char* source = R"(
        fun main(): i32 {
            print("hello");
            print("world");
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "hello\nworld\n");
}

TEST_CASE("E2E - C Backend: Empty string") {
    const char* source = R"(
        fun main(): i32 {
            print("");
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "\n");
}

// --- Step 2: String operations + to_string + f-strings ---

TEST_CASE("E2E - C Backend: String concatenation") {
    const char* source = R"(
        fun main(): i32 {
            var a: string = "hello ";
            var b: string = "world";
            print(a + b);
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - C Backend: String equality") {
    const char* source = R"(
        fun main(): i32 {
            var a: string = "hello";
            var b: string = "hello";
            if (a == b) { return 1; }
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 1);
}

TEST_CASE("E2E - C Backend: String inequality") {
    const char* source = R"(
        fun main(): i32 {
            var a: string = "hello";
            var b: string = "world";
            if (a != b) { return 1; }
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 1);
}

TEST_CASE("E2E - C Backend: F-string interpolation") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            print(f"x = {x}");
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "x = 42\n");
}

TEST_CASE("E2E - C Backend: F-string with multiple expressions") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 10;
            var b: i32 = 20;
            print(f"{a} + {b} = {a + b}");
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 0);
    CHECK(result.stdout_output == "10 + 20 = 30\n");
}

TEST_CASE("E2E - C Backend: String length") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            return str_len(s);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 5);
}

// --- Step 3: New / Delete (heap allocation) ---

TEST_CASE("E2E - C Backend: Heap allocation basic") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 20;
            p.y = 22;
            return p.x + p.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Heap allocation with constructor") {
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
            var p: uniq Point = uniq Point(20, 22);
            return p.x + p.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

// --- Step 4: RefInc / RefDec ---

TEST_CASE("E2E - C Backend: Ref parameter") {
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }
        fun sum(p: ref Point): i32 {
            return p.x + p.y;
        }
        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 20;
            p.y = 22;
            return sum(p);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Ref write through") {
    const char* source = R"(
        struct Counter {
            val: i32;
        }
        fun increment(c: ref Counter) {
            c.val = c.val + 1;
        }
        fun main(): i32 {
            var c: uniq Counter = uniq Counter();
            c.val = 41;
            increment(c);
            return c.val;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

// --- Step 6: List operations ---

TEST_CASE("E2E - C Backend: List push and len") {
    const char* source = R"(
        fun main(): i32 {
            var list: List<i32> = List<i32>();
            list.push(10);
            list.push(20);
            list.push(12);
            return list.len();
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 3);
}

TEST_CASE("E2E - C Backend: List indexing") {
    const char* source = R"(
        fun main(): i32 {
            var list: List<i32> = List<i32>();
            list.push(10);
            list.push(20);
            list.push(12);
            return list[0] + list[1] + list[2];
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: List pop") {
    const char* source = R"(
        fun main(): i32 {
            var list: List<i32> = List<i32>();
            list.push(10);
            list.push(42);
            list.push(99);
            list.pop();
            return list.pop();
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

// --- Step 7: Map operations ---

TEST_CASE("E2E - C Backend: Map insert and get") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 32);
            return m.get(1) + m.get(2);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map contains and remove") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 42);
            if (m.contains(1)) {
                var val: i32 = m.get(1);
                m.remove(1);
                if (!m.contains(1)) {
                    return val;
                }
            }
            return 0;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map len") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            m.insert(3, 30);
            return m.len();
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 3);
}

// ===========================================================================
// Variable-sized container values (ported from VM in roxy_rt.cpp)
// ===========================================================================

TEST_CASE("E2E - C Backend: Map<i32, i64> 2-slot value") {
    // value_slot_count = 2, value_is_inline = 1.
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i64> = Map<i32, i64>();
            m.insert(1, 9000000000l);
            m.insert(2, 33000000000l);
            var v: i64 = m.get(1) + m.get(2);
            return i32(v / 1000000000l);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map<i32, struct> value") {
    // value_slot_count = 2 (Point has 2 i32 fields), value_is_inline = 0.
    const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun main(): i32 {
            var m: Map<i32, Point> = Map<i32, Point>();
            m.insert(1, Point { x = 10, y = 32 });
            var got: Point = m.get(1);
            return got.x + got.y;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map<i32, struct> rehash exercises ping-pong scratch buffers") {
    // Insert 25 entries to cross the 80% load threshold (default capacity 8 →
    // grow to 16 → grow to 32). map_grow re-inserts every entry through
    // map_insert_internal, where the value-being-placed aliases the OLD
    // bucket array — without ping-pong scratch buffers this corrupts swaps.
    const char* source = R"(
        struct Pair { a: i32; b: i32; }
        fun main(): i32 {
            var m: Map<i32, Pair> = Map<i32, Pair>();
            for (var i: i32 = 0; i < 25; i = i + 1) {
                m.insert(i, Pair { a = i, b = i * 2 });
            }
            var p: Pair = m.get(7);
            return p.a + p.b + 21;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: List<struct> element") {
    const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun main(): i32 {
            var lst: List<Point> = List<Point>();
            lst.push(Point { x = 1, y = 10 });
            lst.push(Point { x = 2, y = 20 });
            lst.push(Point { x = 9, y = 0 });
            return lst[0].x + lst[1].y + lst[2].x + 12;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map<i32, i32> negative round-trip") {
    // Sign-extension check: storing negative i32 values and reading them back
    // must preserve the sign. The runtime returns void*; the emitter
    // dereferences as `*(int32_t*)` so C's typed-deref does the right thing.
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, -1);
            m.insert(2, -41);
            var s: i32 = m.get(1) + m.get(2);
            return -s;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

// ===========================================================================
// Struct keys (MapKeyKind::Struct, bytewise hash + memcmp)
// ===========================================================================

TEST_CASE("E2E - C Backend: Map<Struct, i32> basic insert + get") {
    const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun main(): i32 {
            var m: Map<Point, i32> = Map<Point, i32>();
            m.insert(Point { x = 1, y = 2 }, 10);
            m.insert(Point { x = 3, y = 4 }, 32);
            return m.get(Point { x = 1, y = 2 }) + m.get(Point { x = 3, y = 4 });
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map<Struct, Struct>") {
    const char* source = R"(
        struct Pos { x: i32; y: i32; }
        struct Color { r: i32; g: i32; b: i32; }
        fun main(): i32 {
            var m: Map<Pos, Color> = Map<Pos, Color>();
            m.insert(Pos { x = 0, y = 0 }, Color { r = 1, g = 2, b = 3 });
            m.insert(Pos { x = 1, y = 1 }, Color { r = 10, g = 20, b = 30 });
            var c: Color = m.get(Pos { x = 1, y = 1 });
            return c.r + c.g + c.b;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 60);
}

TEST_CASE("E2E - C Backend: Map<Struct, i32> rehash with struct keys") {
    // Insert > 80% load to force map_grow; exercises ping-pong scratch
    // buffers for variable-sized keys. Return a small value (exit codes
    // are 1 byte on Unix).
    const char* source = R"(
        struct Key { a: i32; b: i32; c: i32; }
        fun main(): i32 {
            var m: Map<Key, i32> = Map<Key, i32>();
            for (var i: i32 = 0; i < 25; i = i + 1) {
                m.insert(Key { a = i, b = i * 2, c = i * 3 }, i + 1);
            }
            return m.get(Key { a = 7, b = 14, c = 21 });
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 8);
}

TEST_CASE("E2E - C Backend: Map<Struct, i32> contains + remove") {
    const char* source = R"(
        struct Pair { a: i32; b: i32; }
        fun main(): i32 {
            var m: Map<Pair, i32> = Map<Pair, i32>();
            m.insert(Pair { a = 1, b = 1 }, 100);
            m.insert(Pair { a = 2, b = 2 }, 200);
            m.insert(Pair { a = 3, b = 3 }, 300);
            var has_two: bool = m.contains(Pair { a = 2, b = 2 });
            var missing: bool = m.contains(Pair { a = 9, b = 9 });
            m.remove(Pair { a = 2, b = 2 });
            var bits: i32 = 0;
            if (has_two) bits = bits + 1;
            if (!missing) bits = bits + 2;
            return m.len() * 10 + bits;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 23);  // len_after=2, bits=3 → 23
}

// ===========================================================================
// Custom Hash / Eq dispatch for struct keys (function-pointer dispatch in C
// runtime). The C emitter passes &K__hash / &K__eq to roxy_map_alloc.
// ===========================================================================

TEST_CASE("E2E - C Backend: Map<Struct, i32> custom hash dispatched") {
    const char* source = R"(
        struct Vec2 { x: i32; y: i32; }
        fun Vec2.hash(): u64 for Hash {
            return u64(self.x * 31 + self.y);
        }
        fun main(): i32 {
            var m: Map<Vec2, i32> = Map<Vec2, i32>();
            m.insert(Vec2 { x = 1, y = 2 }, 42);
            return m.get(Vec2 { x = 1, y = 2 });
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Map<Struct, i32> custom eq collapses keys") {
    const char* source = R"(
        struct Vec2 { x: i32; y: i32; }
        fun Vec2.hash(): u64 for Hash {
            return u64(self.x + self.y);
        }
        fun Vec2.eq(other: Vec2): bool for Eq {
            return (self.x == other.x && self.y == other.y) ||
                   (self.x == other.y && self.y == other.x);
        }
        fun main(): i32 {
            var m: Map<Vec2, i32> = Map<Vec2, i32>();
            m.insert(Vec2 { x = 1, y = 2 }, 100);
            m.insert(Vec2 { x = 2, y = 1 }, 200);   // overwrites
            return m.len() * 100 + m.get(Vec2 { x = 1, y = 2 });
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    // 1 * 100 + 200 = 300
    CHECK(result.exit_code == 44);  // 300 % 256 = 44
}

TEST_CASE("E2E - C Backend: Map<Struct, i32> custom hash survives rehash") {
    const char* source = R"(
        struct K { a: i32; b: i32; }
        fun K.hash(): u64 for Hash {
            return u64(self.a * 31 + self.b);  // simple mix
        }
        fun K.eq(other: K): bool for Eq {
            return self.a == other.a && self.b == other.b;
        }
        fun main(): i32 {
            var m: Map<K, i32> = Map<K, i32>();
            for (var i: i32 = 0; i < 30; i = i + 1) {
                m.insert(K { a = i, b = i * 7 }, i + 1);
            }
            return m.get(K { a = 17, b = 119 });
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 18);
}

// ===== Header emission tests (Phase 3 leftovers) =====
//
// The generated `.hpp` is the public API the embedder consumes. It contains
// `pub` struct typedefs, inline C++ method wrappers, `make_<T>` RAII factories,
// and `pub` free-function declarations. Non-`pub` items must NOT leak into it.

TEST_CASE("E2E - C Backend Header: pub struct and method emitted, non-pub omitted") {
    const char* source = R"(
        pub struct Foo {
            x: i32 = 0;
        }
        pub fun Foo.get_x(): i32 {
            return self.x;
        }
        struct Hidden {
            secret: i32 = 0;
        }
        fun Hidden.peek(): i32 {
            return self.secret;
        }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    // Pub struct definition + inline method wrapper present
    CHECK(hpp.find("struct Foo {") != String::npos);
    CHECK(hpp.find("int32_t get_x()") != String::npos);
    CHECK(hpp.find("Foo__get_x(this)") != String::npos);

    // Non-pub struct must not appear
    CHECK(hpp.find("struct Hidden") == String::npos);
    CHECK(hpp.find("Hidden__peek") == String::npos);
}

TEST_CASE("E2E - C Backend Header: pub free function declaration emitted") {
    const char* source = R"(
        pub fun add(a: i32, b: i32): i32 {
            return a + b;
        }
        fun helper(): i32 { return 7; }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    CHECK(hpp.find("int32_t add(") != String::npos);
    CHECK(hpp.find("int32_t helper(") == String::npos);
}

TEST_CASE("E2E - C Backend Header: make_<T> factory for pub struct with pub constructor") {
    const char* source = R"(
        pub struct Player {
            health: i32 = 0;
        }
        pub fun new Player(hp: i32) {
            self.health = hp;
        }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    CHECK(hpp.find("inline roxy::uniq<Player> make_Player(") != String::npos);
    CHECK(hpp.find("roxy_alloc(sizeof(Player)") != String::npos);
    CHECK(hpp.find("Player__new(ptr") != String::npos);
}

TEST_CASE("E2E - C Backend Header: make_<T> factory for pub struct without user constructor") {
    // The synthesized default constructor inherits the struct's pub-ness, so
    // a pub struct with no user `fun new` should still get make_<T>().
    const char* source = R"(
        pub struct Point {
            x: i32 = 0;
            y: i32 = 0;
        }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    CHECK(hpp.find("inline roxy::uniq<Point> make_Point(") != String::npos);
    CHECK(hpp.find("Point__new(ptr") != String::npos);
}

TEST_CASE("E2E - C Backend Header: pub enum typedef emitted") {
    const char* source = R"(
        pub enum Color { Red, Green, Blue }
        enum Hidden { A, B }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    CHECK(hpp.find("Color_Red") != String::npos);
    CHECK(hpp.find("Color_Blue") != String::npos);
    CHECK(hpp.find("Hidden_A") == String::npos);
}

TEST_CASE("E2E - C Backend Header: non-pub method on pub struct omitted from inline wrappers") {
    const char* source = R"(
        pub struct Foo {
            x: i32 = 0;
        }
        pub fun Foo.shown(): i32 { return self.x; }
        fun Foo.hidden(): i32 { return self.x + 1; }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());

    CHECK(hpp.find("int32_t shown()") != String::npos);
    // Non-pub method should not have an inline wrapper (its mangled body lives in .cpp)
    CHECK(hpp.find("int32_t hidden()") == String::npos);
    CHECK(hpp.find("Foo__hidden") == String::npos);
}

TEST_CASE("E2E - C Backend Header: header compiles standalone as valid C++") {
    const char* source = R"(
        pub struct Vec2 {
            x: f32 = 0.0f;
            y: f32 = 0.0f;
        }
        pub fun Vec2.length_sq(): f32 {
            return self.x * self.x + self.y * self.y;
        }
        pub fun new Vec2(x: f32, y: f32) {
            self.x = x;
            self.y = y;
        }
        pub struct Player {
            health: i32 = 0;
        }
        pub fun new Player(hp: i32) {
            self.health = hp;
        }
        pub fun add(a: i32, b: i32): i32 { return a + b; }
        fun main(): i32 { return 0; }
    )";
    CHECK(header_compiles(source));
}

TEST_CASE("E2E - C Backend Header: non-pub struct's methods don't produce orphaned prototypes") {
    // A non-pub struct's methods would create prototypes referencing a type
    // not in the header; emit_header must skip them.
    const char* source = R"(
        struct Hidden {
            x: i32 = 0;
        }
        pub fun Hidden.touch(): i32 { return self.x; }
        fun main(): i32 { return 0; }
    )";
    String hpp = compile_to_hpp(source);
    REQUIRE(!hpp.empty());
    CHECK(hpp.find("Hidden") == String::npos);
    CHECK(header_compiles(source));
}

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

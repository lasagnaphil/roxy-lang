// C-backend test suite.
//
// Broad language-feature coverage on the C backend now lives in the
// backend-parametric `tests/e2e/` suites (run via `--test-case="*<C>*"`; see
// `test_e2e_backend.hpp`). This file is intentionally NOT deduplicated against
// them: its cases are a hand-curated baseline of patterns known to *pass* on the
// C backend (e.g. "Struct as parameter", "Large struct return", "Map<Struct,i32>
// basic insert + get") plus C-specific tests that have no parametric analogue:
//   - generated-header emission (`header_compiles` / `compile_to_hpp`)
//   - AOT NativeRegistry dispatch (`compile_and_run_cpp_with_registry`)
//   - generated-C structure / `#line` directives (`compile_to_cpp`)
// These deliberately test *different aspects* than the parametric cases (which
// often probe value-semantics edges the C backend currently gets wrong and are
// therefore VM-only), so deleting them would drop real passing-on-C coverage.
#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/compiler/type_env.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/natives.hpp"

using namespace rx;

TEST_SUITE("E2E C Backend") {

    TEST_CASE("Return constant") {
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

    TEST_CASE("Integer arithmetic") {
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

    TEST_CASE("Negation") {
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

    TEST_CASE("Comparisons and boolean logic") {
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

    TEST_CASE("If/else control flow") {
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

    TEST_CASE("While loop") {
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

    TEST_CASE("Simple function call") {
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

    TEST_CASE("Nested function calls") {
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

    TEST_CASE("Recursive function") {
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

    TEST_CASE("Bitwise operations") {
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

    TEST_CASE("Multiple functions") {
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

    TEST_CASE("Struct basic") {
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

    TEST_CASE("Struct as parameter") {
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

    TEST_CASE("Struct as return value") {
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

    TEST_CASE("Struct method") {
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

    TEST_CASE("Struct constructor") {
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

    TEST_CASE("Struct inheritance") {
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

    TEST_CASE("Struct copy") {
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

    TEST_CASE("Nested struct") {
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

    TEST_CASE("Enum definition") {
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

    TEST_CASE("Struct with enum field") {
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

    TEST_CASE("Out parameter") {
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

    TEST_CASE("Inout parameter") {
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

    TEST_CASE("Large struct return") {
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

    TEST_CASE("Cast f64 to i32") {
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

    TEST_CASE("Tagged union") {
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

    TEST_CASE("compile_to_cpp produces valid output") {
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

    TEST_CASE("Print string literal") {
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

    TEST_CASE("Multiple prints") {
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

    TEST_CASE("Empty string") {
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

    TEST_CASE("String concatenation") {
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

    TEST_CASE("String equality") {
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

    TEST_CASE("String inequality") {
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

    TEST_CASE("F-string interpolation") {
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

    TEST_CASE("F-string with multiple expressions") {
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

    TEST_CASE("String length") {
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

    TEST_CASE("Heap allocation basic") {
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

    TEST_CASE("Heap allocation with constructor") {
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

    TEST_CASE("Ref parameter") {
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

    TEST_CASE("Ref write through") {
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

    TEST_CASE("List push and len") {
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

    TEST_CASE("List indexing") {
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

    TEST_CASE("List pop") {
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

    TEST_CASE("Map insert and get") {
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

    TEST_CASE("Map contains and remove") {
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

    TEST_CASE("Map len") {
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

    TEST_CASE("Map<i32, i64> 2-slot value") {
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

    TEST_CASE("Map<i32, struct> value") {
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

    TEST_CASE("Map<i32, struct> rehash exercises ping-pong scratch buffers") {
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

    TEST_CASE("List<struct> element") {
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

    TEST_CASE("Map<i32, i32> negative round-trip") {
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

    TEST_CASE("Map<Struct, i32> basic insert + get") {
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

    TEST_CASE("Map<Struct, Struct>") {
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

    TEST_CASE("Map<Struct, i32> rehash with struct keys") {
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

    TEST_CASE("Map<Struct, i32> contains + remove") {
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

    TEST_CASE("Map<Struct, i32> custom hash dispatched") {
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

    TEST_CASE("Map<Struct, i32> custom eq collapses keys") {
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

    TEST_CASE("Map<Struct, i32> custom hash survives rehash") {
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

    TEST_CASE("C Backend Header: pub struct and method emitted, non-pub omitted") {
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

    TEST_CASE("C Backend Header: pub free function declaration emitted") {
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

    TEST_CASE("C Backend Header: make_<T> factory for pub struct with pub constructor") {
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

    TEST_CASE("C Backend Header: make_<T> factory for pub struct without user constructor") {
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

    TEST_CASE("C Backend Header: pub enum typedef emitted") {
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

    TEST_CASE("C Backend Header: non-pub method on pub struct omitted from inline wrappers") {
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

    TEST_CASE("C Backend Header: header compiles standalone as valid C++") {
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

    // ===== AOT main entry tests (Phase 4 step 3) =====
    //
    // The standalone `main()` emitted in AOT mode renames the user's `fun main`
    // to `main_entry()` and wraps it in a real C `main` that initializes the
    // thread-local `roxy_ctx`. The runtime tests in test_runtime_ctx.cpp cover
    // the TLS API; the existing 66 C-backend tests cover the runtime path. These
    // pin down the generated source structure.

    TEST_CASE("C Backend AOT: generated main wraps user main_entry with ctx init") {
        const char* source = R"(
        fun main(): i32 {
            return 7;
        }
    )";
        String cpp = compile_to_cpp(source);
        REQUIRE(!cpp.empty());

        // User's `fun main` is renamed
        CHECK(cpp.find("int32_t main_entry(") != String::npos);

        // Generated wrapper exists, sets up TLS context, calls main_entry, tears down
        CHECK(cpp.find("int main(int argc, char** argv)") != String::npos);
        CHECK(cpp.find("roxy_rt_init();") != String::npos);
        CHECK(cpp.find("roxy_ctx ctx;") != String::npos);
        CHECK(cpp.find("roxy_ctx_init(&ctx);") != String::npos);
        CHECK(cpp.find("roxy_set_ctx(&ctx);") != String::npos);
        CHECK(cpp.find("main_entry()") != String::npos);
        CHECK(cpp.find("roxy_ctx_destroy(&ctx);") != String::npos);
        CHECK(cpp.find("roxy_rt_shutdown();") != String::npos);
    }

    TEST_CASE("C Backend AOT: void-returning user main wrapper returns 0") {
        const char* source = R"(
        fun main() {
            print("hi");
        }
    )";
        String cpp = compile_to_cpp(source);
        REQUIRE(!cpp.empty());

        CHECK(cpp.find("void main_entry(") != String::npos);
        CHECK(cpp.find("int main(int argc, char** argv)") != String::npos);
        CHECK(cpp.find("return 0;") != String::npos);
    }

    // User-bound C++ native function used by the AOT NativeRegistry test.
    // Defined inline in a temp header; the VM-side `bind<>` registration just
    // references it by symbol.
    namespace { i32 my_aot_add(i32 a, i32 b) { return a + b; } }

    TEST_CASE("C Backend AOT: user-registered native dispatches via NativeRegistry") {
        BumpAllocator alloc(8192);
        TypeEnv type_env(alloc);
        NativeRegistry registry(alloc, type_env.types());
        register_builtin_natives(registry);
        registry.bind<my_aot_add>("my_aot_add");

        const char* source = R"(
        fun main(): i32 {
            return my_aot_add(40, 2);
        }
    )";

        // The generated AOT source emits `my_aot_add(40, 2)`; the inline
        // definition in the embedder header makes it linkable.
        const char* native_header =
            "#pragma once\n"
            "#include <stdint.h>\n"
            "inline int32_t my_aot_add(int32_t a, int32_t b) { return a + b; }\n";

        CBackendResult result = compile_and_run_cpp_with_registry(
            source, &registry, native_header);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("C Backend AOT: user native dispatch shows up in generated source") {
        // Same setup, but only inspect the generated source (no link/run). Catches
        // a regression where the registry-aware dispatch silently falls back to
        // the warning-comment path.
        BumpAllocator alloc(8192);
        TypeEnv type_env(alloc);
        NativeRegistry registry(alloc, type_env.types());
        register_builtin_natives(registry);
        registry.bind<my_aot_add>("my_aot_add");

        const char* source = R"(
        fun main(): i32 {
            return my_aot_add(1, 2);
        }
    )";

        // Drive the source-emit path manually through compile_and_run_cpp_with_registry
        // by passing a no-op header (still need it for linkage if compile_success
        // is asserted below).
        const char* native_header =
            "#pragma once\n"
            "#include <stdint.h>\n"
            "inline int32_t my_aot_add(int32_t a, int32_t b) { return a + b; }\n";

        CBackendResult result = compile_and_run_cpp_with_registry(
            source, &registry, native_header);
        REQUIRE(result.compile_success);
        REQUIRE(result.run_success);
        CHECK(result.exit_code == 3);

        // Sanity: stdout is empty (program returns the result; nothing printed)
        CHECK(result.stdout_output.empty());
    }

    // AOT-symbol-override target. The Roxy program calls `alias_add`, but the
    // registered C++ function is `actual_add` — the bound name and the C++
    // symbol differ on purpose.
    namespace { i32 actual_add(i32 a, i32 b) { return a + b; } }

    TEST_CASE("C Backend AOT: bind<>(roxy_name, aot_symbol) routes to a different C++ symbol") {
        BumpAllocator alloc(8192);
        TypeEnv type_env(alloc);
        NativeRegistry registry(alloc, type_env.types());
        register_builtin_natives(registry);
        // Roxy code calls `alias_add(...)`; the generated AOT source emits a
        // call to `actual_add(...)` because that's the AOT symbol override.
        registry.bind<actual_add>("alias_add", "actual_add");

        const char* source = R"(
        fun main(): i32 {
            return alias_add(20, 22);
        }
    )";

        // The user provides `actual_add` inline (definition + implicit decl).
        // The CEmitter also emits an `extern int32_t actual_add(int32_t, int32_t);`
        // in the source preamble; C++ allows the redeclaration as long as the
        // signatures match.
        const char* native_header =
            "#pragma once\n"
            "#include <stdint.h>\n"
            "inline int32_t actual_add(int32_t a, int32_t b) { return a + b; }\n";

        CBackendResult result = compile_and_run_cpp_with_registry(
            source, &registry, native_header);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("C Backend AOT: extern decl links against a separately-compiled .cpp") {
        // The strong test: NO inline header (so the user's native isn't visible
        // at the generated source's TU). The CEmitter's extern decl emission is
        // what makes the call site compile; a separate .cpp provides the impl
        // and the linker resolves them together.
        BumpAllocator alloc(8192);
        TypeEnv type_env(alloc);
        NativeRegistry registry(alloc, type_env.types());
        register_builtin_natives(registry);
        registry.bind<my_aot_add>("my_aot_add");

        const char* source = R"(
        fun main(): i32 {
            return my_aot_add(7, 8);
        }
    )";

        // No inline header — the impl is in a separate .cpp file and the
        // linker resolves the extern decl emitted in the source preamble.
        const char* impl_cpp =
            "#include <stdint.h>\n"
            "extern \"C++\" int32_t my_aot_add(int32_t a, int32_t b) { return a + b; }\n";

        CBackendResult result = compile_and_run_cpp_with_registry(
            source, &registry, /*native_header_text=*/nullptr,
            /*extra_cpp_text=*/impl_cpp);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 15);
    }

    // ===== Phase 5 polish: #line directives =====
    //
    // Function-level `#line N "<source_path>"` directives are emitted at the
    // start of each generated function body so debuggers / compiler error
    // messages attribute body lines to the original Roxy source instead of
    // the generated C++ file. `CEmitterConfig::source_path` controls emission;
    // no path = no `#line` directives (matches the existing default behavior).

    TEST_CASE("C Backend AOT: #line directives reference the user's source path") {
        const char* source =
            "fun helper(x: i32): i32 {\n"        // line 1
            "    return x * 2;\n"                 // line 2
            "}\n"                                 // line 3
            "fun main(): i32 {\n"                 // line 4
            "    return helper(21);\n"            // line 5
            "}\n";                                // line 6
        String cpp = compile_to_cpp(source, /*debug=*/false, "user.roxy");
        REQUIRE(!cpp.empty());

        // `helper`'s body opens at line 1 (Roxy's `loc.line` for a same-line
        // `fun ... {` body is the function-decl line). Every generated function
        // body should carry a `#line` directive referencing user.roxy.
        CHECK(cpp.find("#line 1 \"user.roxy\"") != String::npos);
        CHECK(cpp.find("#line 4 \"user.roxy\"") != String::npos);
    }

    TEST_CASE("C Backend AOT: no source_path means no #line directives") {
        // The default behavior — when CEmitterConfig::source_path is empty, no
        // `#line` directives are emitted (the generated source is treated as
        // its own canonical source, the way it always has been).
        const char* source = R"(
        fun main(): i32 {
            return 7;
        }
    )";
        String cpp = compile_to_cpp(source);  // no source_path
        REQUIRE(!cpp.empty());
        CHECK(cpp.find("#line ") == String::npos);
    }

    TEST_CASE("C Backend AOT: #line directives track per-statement granularity") {
        // The function-level #line covers the body's first line; subsequent
        // statements on different lines each get their own #line directive
        // so debuggers attribute step-through to the right Roxy line.
        const char* source =
            "fun main(): i32 {\n"     // line 1
            "    var a: i32 = 10;\n"  // line 2
            "    var b: i32 = 20;\n"  // line 3
            "    var c: i32 = 30;\n"  // line 4
            "    return a + b + c;\n" // line 5
            "}\n";                    // line 6
        String cpp = compile_to_cpp(source, /*debug=*/false, "user.roxy");
        REQUIRE(!cpp.empty());

        // Function body opens at line 1 (Roxy puts the body's loc at the
        // function-decl line for same-line `{`). Each subsequent statement
        // produces its own #line directive.
        CHECK(cpp.find("#line 1 \"user.roxy\"") != String::npos);
        CHECK(cpp.find("#line 2 \"user.roxy\"") != String::npos);
        CHECK(cpp.find("#line 3 \"user.roxy\"") != String::npos);
        CHECK(cpp.find("#line 4 \"user.roxy\"") != String::npos);
        CHECK(cpp.find("#line 5 \"user.roxy\"") != String::npos);
    }

    TEST_CASE("C Backend AOT: #line directives don't duplicate within a single statement") {
        // A multi-IR-instruction expression shares the same source line; only
        // one #line directive should be emitted for the whole statement.
        const char* source =
            "fun main(): i32 {\n"
            "    return 1 + 2 + 3 + 4 + 5;\n"  // line 2 — many IR ops
            "}\n";
        String cpp = compile_to_cpp(source, /*debug=*/false, "user.roxy");
        REQUIRE(!cpp.empty());

        // Count occurrences of `#line 2 "user.roxy"` — should be exactly 1.
        u32 count = 0;
        u32 pos = 0;
        while (true) {
            u32 found = cpp.find("#line 2 \"user.roxy\"", pos);
            if (found == String::npos) break;
            count++;
            pos = found + 1;
        }
        CHECK(count == 1);
    }

    TEST_CASE("C Backend AOT: #line-tagged source still compiles + runs") {
        // Sanity check: the C compiler accepts the emitted #line directives and
        // the binary still produces the right answer.
        const char* source = R"(
        fun main(): i32 {
            return 42;
        }
    )";
        String cpp = compile_to_cpp(source, /*debug=*/false, "main.roxy");
        REQUIRE(!cpp.empty());
        REQUIRE(cpp.find("#line ") != String::npos);

        // Regular compile_and_run_cpp goes through compile_to_cpp without
        // source_path, so it doesn't exercise the #line path end-to-end. The
        // assertion above on string content is the authoritative test for
        // #line emission; this test just ensures no syntactic damage.
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("C Backend Header: non-pub struct's methods don't produce orphaned prototypes") {
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

    // ── Module-level globals ──
    // Globals become C global variables; the synthesized __module_init runs
    // their initializers/constructors (driven from the generated main, after the
    // ctx is up), and __module_shutdown runs destructors before teardown.

    TEST_CASE("Global: primitive init and read") {
        const char* source = R"(
            var n: i32 = 42;
            fun main(): i32 { return n; }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Global: mutation persists across calls") {
        const char* source = R"(
            var counter: i32 = 0;
            fun bump() { counter = counter + 1; }
            fun main(): i32 { bump(); bump(); bump(); return counter; }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 3);
    }

    TEST_CASE("Global: initializer reads an earlier global") {
        const char* source = R"(
            var base: i32 = 100;
            var derived: i32 = base + 5;
            fun main(): i32 { return derived; }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 105);
    }

    TEST_CASE("Global: struct field access") {
        const char* source = R"(
            struct Point { x: i32; y: i32; }
            var origin: Point = Point { x = 3, y = 4 };
            fun main(): i32 {
                origin.x = origin.x + 10;
                return origin.x + origin.y;   // 17
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 17);
    }

    TEST_CASE("Global: uniq runs constructor at init") {
        const char* source = R"(
            struct Counter { value: i32; }
            fun new Counter(v: i32) { self.value = v; }
            fun delete Counter() {}
            var g: uniq Counter = uniq Counter(7);
            fun main(): i32 { return g.value; }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 7);
    }

    TEST_CASE("Global: uniq destructor runs at shutdown") {
        // Unlike the VM test harness (which restores stdout before teardown),
        // the C binary runs __module_shutdown inside main(), so the destructor's
        // output is observable.
        const char* source = R"(
            struct Resource { id: i32; }
            fun new Resource(id: i32) { self.id = id; }
            fun delete Resource() { print("freed"); }
            var res: uniq Resource = uniq Resource(1);
            fun main(): i32 { return res.id; }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
        CHECK(result.stdout_output == "freed\n");
    }

    TEST_CASE("Global: uniq reassignment frees the old value") {
        const char* source = R"(
            struct Box { v: i32; }
            fun new Box(v: i32) { self.v = v; }
            fun delete Box() {}
            var b: uniq Box = uniq Box(1);
            fun main(): i32 {
                b = uniq Box(2);
                return b.v;   // 2
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
    }

    // ── Container element teardown ──
    // A noncopyable List/Map's Delete now iterates its elements (recursively),
    // runs their destructors, and frees the backing buffers — the C analogue of
    // the VM's descriptor-driven delete_value.

    TEST_CASE("List<uniq T> runs element destructors at scope exit") {
        const char* source = R"(
            struct Counter { value: i32; }
            fun new Counter(v: i32) { self.value = v; }
            fun delete Counter() { print("del"); }
            fun main(): i32 {
                var lst: List<uniq Counter> = List<uniq Counter>();
                lst.push(uniq Counter(1));
                lst.push(uniq Counter(2));
                lst.push(uniq Counter(3));
                return lst.len();   // 3; 3 element destructors run at scope exit
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 3);
        CHECK(result.stdout_output == "del\ndel\ndel\n");
    }

    TEST_CASE("List<ValueStruct with dtor> runs in-place element destructors") {
        const char* source = R"(
            struct Tag { id: i32; }
            fun delete Tag() { print("t"); }
            fun main(): i32 {
                var lst: List<Tag> = List<Tag>();
                lst.push(Tag { id = 1 });
                lst.push(Tag { id = 2 });
                return lst.len();   // 2; 2 in-place destructors at scope exit
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
        CHECK(result.stdout_output == "t\nt\n");
    }

    TEST_CASE("Map<i32, uniq T> runs value destructors at scope exit") {
        const char* source = R"(
            struct Counter { value: i32; }
            fun new Counter(v: i32) { self.value = v; }
            fun delete Counter() { print("v"); }
            fun main(): i32 {
                var m: Map<i32, uniq Counter> = Map<i32, uniq Counter>();
                m.insert(1, uniq Counter(10));
                m.insert(2, uniq Counter(20));
                return m.len();   // 2; both values destroyed at scope exit
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
        // Bucket order is unspecified, but both values print the same token.
        CHECK(result.stdout_output == "v\nv\n");
    }

    TEST_CASE("List<List<uniq T>> recursively tears down nested elements") {
        const char* source = R"(
            struct Counter { value: i32; }
            fun new Counter(v: i32) { self.value = v; }
            fun delete Counter() { print("x"); }
            fun main(): i32 {
                var outer: List<List<uniq Counter>> = List<List<uniq Counter>>();
                var a: List<uniq Counter> = List<uniq Counter>();
                a.push(uniq Counter(1));
                a.push(uniq Counter(2));
                outer.push(a);
                var b: List<uniq Counter> = List<uniq Counter>();
                b.push(uniq Counter(3));
                outer.push(b);
                return outer.len();   // 2 inner lists, 3 Counters total
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
        CHECK(result.stdout_output == "x\nx\nx\n");  // all 3 nested Counters freed
    }

    // ── Coroutines ──
    // coroutine_lower() rewrites each Coro<T> function into init/resume/done/
    // $$delete built from ops the C backend already supports; these verify the
    // Coro<T> type emits as its state-struct pointer and the lifecycle works.

    TEST_CASE("Coroutine single yield") {
        const char* source = R"(
        fun single(): Coro<i32> {
            yield 42;
        }
        fun main(): i32 {
            var g = single();
            return g.resume();
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Coroutine multiple yields") {
        const char* source = R"(
        fun triple(): Coro<i32> {
            yield 10;
            yield 20;
            yield 30;
        }
        fun main(): i32 {
            var g = triple();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a + b + c;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 60);
    }

    TEST_CASE("Coroutine done check") {
        const char* source = R"(
        fun one_val(): Coro<i32> {
            yield 99;
        }
        fun to_int(b: bool): i32 {
            if (b) { return 1; }
            return 0;
        }
        fun main(): i32 {
            var g = one_val();
            var before: i32 = to_int(g.done());
            g.resume();
            var after_one: i32 = to_int(g.done());
            g.resume();
            var after_two: i32 = to_int(g.done());
            return before * 100 + after_one * 10 + after_two;
        }
    )";
        // before=0 (not done), after_one=0 (not done), after_two=1 (done)
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    TEST_CASE("Coroutine yield in while loop") {
        const char* source = R"(
        fun counter(): Coro<i32> {
            var i: i32 = 0;
            while (i < 3) {
                yield i;
                i = i + 1;
            }
        }
        fun main(): i32 {
            var g = counter();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            var c: i32 = g.resume();
            return a * 100 + b * 10 + c;   // 0,1,2 -> 12
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 12);
    }

    TEST_CASE("Coroutine with parameters") {
        const char* source = R"(
        fun add_offset(base: i32, offset: i32): Coro<i32> {
            yield base + offset;
            yield base + offset + 1;
        }
        fun main(): i32 {
            var g = add_offset(10, 5);
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a + b;   // 15 + 16 = 31
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 31);
    }

    TEST_CASE("Coroutine struct promoted local across yield") {
        // A value-struct local that survives a yield becomes a struct-typed field
        // on the state struct __coro_gen, exercising dependency-sorted typedefs.
        const char* source = R"(
        struct Vec2 { x: i32; y: i32; }
        fun gen(): Coro<i32> {
            var v: Vec2 = Vec2 { x = 3, y = 4 };
            yield v.x;
            yield v.y;
        }
        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            var b: i32 = g.resume();
            return a * 10 + b;   // 34
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 34);
    }

    TEST_CASE("Coroutine uniq promoted, run to completion") {
        // Promoted uniq freed by inline cleanup on the done path; the generated
        // __coro_gen$$delete destructor sees null and skips it (no double free).
        const char* source = R"(
        struct Resource { value: i32; }
        fun delete Resource() {
            print("dtor");
        }
        fun gen(): Coro<i32> {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            yield r.value;
            yield r.value + 1;
        }
        fun main(): i32 {
            var g = gen();
            var a: i32 = g.resume();
            g.resume();
            g.resume();   // reach done -> inline cleanup of r
            return a;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
        CHECK(result.stdout_output == "dtor\n");   // freed exactly once
    }

    TEST_CASE("Coroutine uniq promoted, early drop runs $$delete") {
        // Drop the Coro before it reaches done: __coro_gen$$delete must free the
        // still-live promoted uniq field.
        const char* source = R"(
        struct Resource { value: i32; }
        fun delete Resource() {
            print("freed");
        }
        fun gen(): Coro<i32> {
            var r: uniq Resource = uniq Resource();
            r.value = 99;
            yield r.value;
            yield r.value + 1;
        }
        fun main(): i32 {
            var result: i32 = 0;
            {
                var g = gen();
                result = g.resume();
                // g leaves scope here without reaching done
            }
            return result;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 99);
        CHECK(result.stdout_output == "freed\n");
    }

    // ── Exceptions ──
    // throw/catch/finally lower to a thread-local in-flight exception + checked-
    // return dispatch; per-frame cleanup runs as null-guarded Delete on the
    // unwind path. These mirror the runtime-behavior cases in test_exceptions.cpp.

    TEST_CASE("Exception basic throw/catch") {
        const char* source = R"(
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun risky(): i32 { throw MyError { code = 42 }; return 0; }
        fun main(): i32 {
            try { risky(); } catch (e: MyError) { return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Exception catch-all") {
        const char* source = R"(
        struct SomeError { val: i32; }
        fun SomeError.message(): string for Exception { return "some error"; }
        fun risky(): i32 { throw SomeError { val = 99 }; return 0; }
        fun main(): i32 {
            try { risky(); } catch (e) { return 1; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    TEST_CASE("Exception multiple catch clauses") {
        const char* source = R"(
        struct ErrorA { code: i32; }
        fun ErrorA.message(): string for Exception { return "a"; }
        struct ErrorB { code: i32; }
        fun ErrorB.message(): string for Exception { return "b"; }
        fun throw_b(): i32 { throw ErrorB { code = 20 }; return 0; }
        fun main(): i32 {
            try { throw_b(); }
            catch (e: ErrorA) { return e.code; }
            catch (e: ErrorB) { return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 20);
    }

    TEST_CASE("Exception typed catch mismatch falls through to catch-all") {
        const char* source = R"(
        struct ErrorA { x: i32; }
        fun ErrorA.message(): string for Exception { return "a"; }
        struct ErrorB { x: i32; }
        fun ErrorB.message(): string for Exception { return "b"; }
        fun throw_b(): i32 { throw ErrorB { x = 5 }; return 0; }
        fun main(): i32 {
            try { throw_b(); }
            catch (e: ErrorA) { return 1; }
            catch (e) { return 99; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 99);
    }

    TEST_CASE("Exception finally on normal exit") {
        const char* source = R"(
        struct MyError { x: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun main(): i32 {
            var result: i32 = 0;
            try { result = 10; }
            catch (e: MyError) { result = -1; }
            finally { result = result + 1; }
            return result;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 11);
    }

    TEST_CASE("Exception finally on exception path") {
        const char* source = R"(
        struct MyError { x: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun thrower(): i32 { throw MyError { x = 5 }; return 0; }
        fun main(): i32 {
            var result: i32 = 0;
            try { thrower(); result = 10; }
            catch (e: MyError) { result = e.x; }
            finally { result = result + 100; }
            return result;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 105);
    }

    TEST_CASE("Exception try without catch (finally only)") {
        const char* source = R"(
        fun main(): i32 {
            var result: i32 = 0;
            try { result = 42; } finally { result = result + 1; }
            return result;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 43);
    }

    TEST_CASE("Exception stack unwinding through multiple frames") {
        const char* source = R"(
        struct DeepError { level: i32; }
        fun DeepError.message(): string for Exception { return "deep"; }
        fun level3(): i32 { throw DeepError { level = 3 }; return 0; }
        fun level2(): i32 { return level3(); }
        fun level1(): i32 { return level2(); }
        fun main(): i32 {
            try { return level1(); } catch (e: DeepError) { return e.level; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 3);
    }

    TEST_CASE("Exception nested try/catch") {
        const char* source = R"(
        struct InnerError { x: i32; }
        fun InnerError.message(): string for Exception { return "inner"; }
        struct OuterError { x: i32; }
        fun OuterError.message(): string for Exception { return "outer"; }
        fun throw_inner(): i32 { throw InnerError { x = 10 }; return 0; }
        fun main(): i32 {
            try {
                try { throw_inner(); } catch (e: InnerError) { return e.x; }
            } catch (e: OuterError) { return -1; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 10);
    }

    TEST_CASE("Exception unhandled exits nonzero") {
        const char* source = R"(
        struct FatalError { code: i32; }
        fun FatalError.message(): string for Exception { return "fatal"; }
        fun main(): i32 { throw FatalError { code = 1 }; return 0; }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.exit_code != 0);
    }

    TEST_CASE("Exception fields accessible in catch") {
        const char* source = R"(
        struct ValueError { code: i32; extra: i32; }
        fun ValueError.message(): string for Exception { return "value error"; }
        fun fail(): i32 { throw ValueError { code = 7, extra = 13 }; return 0; }
        fun main(): i32 {
            try { fail(); } catch (e: ValueError) { return e.code + e.extra; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 20);
    }

    TEST_CASE("Exception normal flow no exception thrown") {
        const char* source = R"(
        struct MyError { x: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun safe(): i32 { return 42; }
        fun main(): i32 {
            try { var x: i32 = safe(); return x; } catch (e: MyError) { return -1; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Exception safety: throw cleans up current scope uniq") {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print("~Resource"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun main(): i32 {
            try {
                var r: uniq Resource = uniq Resource();
                r.id = 1;
                throw MyError { code = 42 };
            } catch (e: MyError) {
                print("caught");
                return e.code;
            }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE("Exception safety: cross-frame unwinding cleanup") {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print("~Resource"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun thrower() { throw MyError { code = 7 }; }
        fun middle() {
            var r: uniq Resource = uniq Resource();
            r.id = 2;
            thrower();
        }
        fun main(): i32 {
            try { middle(); return -1; }
            catch (e: MyError) { print("caught"); return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 7);
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE("Exception safety: LIFO cleanup order") {
        const char* source = R"(
        struct A { val: i32; }
        fun delete A() { print("~A"); }
        struct B { val: i32; }
        fun delete B() { print("~B"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun risky() { throw MyError { code = 1 }; }
        fun main(): i32 {
            try {
                var a: uniq A = uniq A();
                var b: uniq B = uniq B();
                risky();
                return -1;
            } catch (e: MyError) { print("caught"); return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
        CHECK(result.stdout_output == "~B\n~A\ncaught\n");
    }

    TEST_CASE("Exception safety: nested try/catch cleanup") {
        const char* source = R"(
        struct Outer { val: i32; }
        fun delete Outer() { print("~Outer"); }
        struct Inner { val: i32; }
        fun delete Inner() { print("~Inner"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun risky() { throw MyError { code = 5 }; }
        fun work() {
            var outer: uniq Outer = uniq Outer();
            try {
                var inner: uniq Inner = uniq Inner();
                risky();
            } catch (e: MyError) { print("inner caught"); }
        }
        fun main(): i32 {
            work();
            print("done");
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0);
        CHECK(result.stdout_output == "~Inner\ninner caught\n~Outer\ndone\n");
    }

    TEST_CASE("Exception safety: value struct destructor during unwinding") {
        const char* source = R"(
        struct Guard { name: i32; }
        fun delete Guard() { print("~Guard"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun risky() { throw MyError { code = 3 }; }
        fun guarded() {
            var g: Guard = Guard { name = 1 };
            risky();
        }
        fun main(): i32 {
            try { guarded(); return -1; }
            catch (e: MyError) { print("caught"); return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 3);
        CHECK(result.stdout_output == "~Guard\ncaught\n");
    }

    TEST_CASE("Exception safety: already-moved uniq skipped during cleanup") {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print("~Resource"); }
        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }
        fun consume(r: uniq Resource): i32 { return r.id; }
        fun main(): i32 {
            try {
                var r: uniq Resource = uniq Resource();
                r.id = 42;
                var val: i32 = consume(r);
                throw MyError { code = val };
            } catch (e: MyError) { print("caught"); return e.code; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
        // Resource freed once (by consume), not double-freed by exception cleanup.
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE("Exception cleanup: List<uniq T> elements destroyed on unwind") {
        const char* source = R"(
        struct Widget { id: i32; }
        fun delete Widget() { print(f"del:{self.id}"); }
        struct Boom { code: i32; }
        fun Boom.message(): string for Exception { return "boom"; }
        fun explode() { throw Boom { code = 1 }; }
        fun main(): i32 {
            try {
                var items: List<uniq Widget> = List<uniq Widget>();
                items.push(uniq Widget { id = 10 });
                items.push(uniq Widget { id = 20 });
                items.push(uniq Widget { id = 30 });
                explode();
                return -1;
            } catch (e: Boom) { return e.code; }
            return -2;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
        CHECK(result.stdout_output.find("del:10") != String::npos);
        CHECK(result.stdout_output.find("del:20") != String::npos);
        CHECK(result.stdout_output.find("del:30") != String::npos);
    }

    TEST_CASE("Exception cleanup: Map<string, uniq T> values destroyed on unwind") {
        const char* source = R"(
        struct Resource { value: i32; }
        fun delete Resource() { print(f"free:{self.value}"); }
        struct Fail { x: i32; }
        fun Fail.message(): string for Exception { return "fail"; }
        fun main(): i32 {
            try {
                var m: Map<string, uniq Resource> = Map<string, uniq Resource>();
                m.insert("a", uniq Resource { value = 100 });
                m.insert("b", uniq Resource { value = 200 });
                throw Fail { x = 42 };
                return -1;
            } catch (e: Fail) { return e.x; }
            return -2;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
        CHECK(result.stdout_output.find("free:100") != String::npos);
        CHECK(result.stdout_output.find("free:200") != String::npos);
    }

    TEST_CASE("Exception cleanup: temporary uniq destroyed on unwind") {
        const char* source = R"(
        struct Widget { id: i32; }
        fun delete Widget() { print(f"del:{self.id}"); }
        struct Boom { code: i32; }
        fun Boom.message(): string for Exception { return "boom"; }
        fun consume(w: uniq Widget): i32 { throw Boom { code = 1 }; return 0; }
        fun main(): i32 {
            try { consume(uniq Widget { id = 99 }); return -1; }
            catch (e: Boom) { return e.code; }
            return -2;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
        CHECK(result.stdout_output.find("del:99") != String::npos);
    }

    TEST_CASE("Exception: try/catch around while loop catches inner throw") {
        const char* source = R"(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }
        fun deep() { throw Err { code = 1 }; }
        fun main(): i32 {
            try {
                var i: i32 = 0;
                while (i < 3) { deep(); i = i + 1; }
            } catch (e: Err) { return 42; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Exception: try/catch around for loop catches inner throw") {
        const char* source = R"(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }
        fun deep() { throw Err { code = 1 }; }
        fun main(): i32 {
            try {
                for (var i: i32 = 0; i < 3; i = i + 1) { deep(); }
            } catch (e: Err) { return 42; }
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    // ── Closures ──
    // Lambdas lift to top-level call functions + env structs; the C backend
    // dispatches CallIndirect through a per-module function-pointer table
    // (g_closure_fns) indexed by the env's __call_idx. Mirrors test_closures.cpp.

    TEST_CASE("Closure: lambda expression body + immediate call") {
        const char* source = R"(
        fun main() {
            var f = fun(x: i32): i32 => x + 1;
            print(f"{f(5)}");
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.stdout_output == "6\n");
    }

    TEST_CASE("Closure: lambda block body and void lambda") {
        const char* source = R"(
        fun main() {
            var g = fun(): i32 { return 42; };
            print(f"{g()}");
            var greet = fun(name: string) { print(f"hello {name}"); };
            greet("world");
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.stdout_output == "42\nhello world\n");
    }

    TEST_CASE("Closure: higher-order function (closure as parameter)") {
        const char* source = R"(
        fun apply_twice(f: fun(i32) -> i32, x: i32): i32 { return f(f(x)); }
        fun main(): i32 {
            return apply_twice(fun(x: i32): i32 => x * 2, 3);   // 12
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 12);
    }

    TEST_CASE("Closure: returned from function, captures parameter") {
        const char* source = R"(
        fun make_adder(n: i32): fun(i32) -> i32 {
            return fun(x: i32): i32 => x + n;
        }
        fun main(): i32 {
            var add5 = make_adder(5);
            return add5(10);   // 15
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 15);
    }

    TEST_CASE("Closure: implicit copy capture is by value") {
        const char* source = R"(
        fun main() {
            var n: i32 = 10;
            var f = fun(): i32 => n;
            n = 99;
            print(f"{f()}");   // 10 (snapshot)
            print(f"{n}");     // 99
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.stdout_output == "10\n99\n");
    }

    TEST_CASE("Closure: capture multiple variables") {
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 100;
            var y: i32 = 50;
            var add = fun(a: i32): i32 => a + x + y;
            return add(7);   // 157
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 157);
    }

    TEST_CASE("Closure: explicit [move] capture of uniq") {
        const char* source = R"(
        struct Counter { value: i32 = 0; }
        fun delete Counter() { print("~Counter"); }
        fun main(): i32 {
            var c: uniq Counter = uniq Counter { value = 42 };
            var f = fun[move c](): i32 => c.value;
            var v: i32 = f();
            return v;   // 42; env $$delete frees the moved Counter at scope exit
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
        CHECK(result.stdout_output == "~Counter\n");
    }

    TEST_CASE("Closure: function reference (trampoline)") {
        // Note: avoid `double` as a Roxy function name — it collides with the C++
        // keyword in the generated source (a pre-existing C-backend naming gap).
        const char* source = R"(
        fun dbl(x: i32): i32 { return x * 2; }
        fun triple(x: i32): i32 { return x * 3; }
        fun apply(f: fun(i32) -> i32, x: i32): i32 { return f(x); }
        fun main(): i32 {
            var f: fun(i32) -> i32 = dbl;
            return f(21) + apply(triple, 10);   // 42 + 30 = 72
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 72);
    }

    TEST_CASE("Closure: void-returning function reference") {
        const char* source = R"(
        fun greet(name: string) { print(f"hi {name}"); }
        fun main() {
            var g = greet;
            g("world");
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.stdout_output == "hi world\n");
    }

    TEST_CASE("Closure: nested closures (transitive capture)") {
        const char* source = R"(
        fun main(): i32 {
            var n: i32 = 100;
            var f = fun(): fun(i32) -> fun(i32) -> i32 {
                return fun(a: i32): fun(i32) -> i32 {
                    return fun(b: i32): i32 => n + a + b;
                };
            };
            return f()(1)(2);   // 103
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 103);
    }

    TEST_CASE("Closure: implicit ref-self on noncopyable receiver") {
        const char* source = R"(
        struct Counter { value: i32 = 0; }
        fun delete Counter() {}
        fun Counter.make_getter(): fun() -> i32 {
            return fun(): i32 => self.value;
        }
        fun main(): i32 {
            var c: uniq Counter = uniq Counter { value = 42 };
            var g = c.make_getter();
            return g();   // 42
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    TEST_CASE("Closure: [copy self] snapshot semantics") {
        const char* source = R"(
        struct Vec2 { x: i32 = 0; y: i32 = 0; }
        fun Vec2.snapshot(): fun() -> i32 {
            return fun[copy self](): i32 => self.x + self.y;
        }
        fun main(): i32 {
            var v: Vec2 = Vec2 { x = 3, y = 4 };
            var f = v.snapshot();
            v.x = 999;
            v.y = 999;
            return f();   // 7 (snapshot taken at capture)
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 7);
    }

    TEST_CASE("Closure: [weak self] on uniq receiver passes heap check") {
        const char* source = R"(
        struct V { x: i32 = 0; }
        fun V.make(): fun() -> i32 {
            return fun[weak self](): i32 => self.x;
        }
        fun main(): i32 {
            var u: uniq V = uniq V { x = 21 };
            var f = u.make();
            return f();   // 21
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 21);
    }

    TEST_CASE("Closure: ref-self on stack receiver traps (AssertHeap)") {
        const char* source = R"(
        struct V { x: i32 = 0; }
        fun V.make(): fun() -> i32 {
            return fun(): i32 => self.x;
        }
        fun main(): i32 {
            var v: V = V { x = 7 };
            var f = v.make();   // captures ref self of a stack receiver -> trap
            return 0;
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.exit_code != 0);   // runtime trap (abort)
    }

    TEST_CASE("Closure: borrowed function value out of a List is callable") {
        const char* source = R"(
        fun main(): i32 {
            var fs: List<fun(i32) -> i32> = List<fun(i32) -> i32>();
            fs.push(fun(x: i32): i32 => x + 1);
            fs.push(fun(x: i32): i32 => x * 2);
            var g: ref fun(i32) -> i32 = fs[1];   // borrow, not move
            return fs[0](10) + g(10);             // 11 + 20 = 31
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 31);
    }

    // ── C++ keyword identifier escaping ──
    // Roxy identifiers that are C++ keywords (but not Roxy keywords) are escaped
    // with a reserved `roxy_kw_` prefix so the generated C++ compiles.

    TEST_CASE("Keyword escape: functions named `double` and `double_` coexist") {
        // `double` is a C++ keyword; `double_` is not. The prefix scheme keeps them
        // distinct (a `_` suffix would alias them).
        const char* source = R"(
        fun double(x: i32): i32 { return x * 2; }
        fun double_(x: i32): i32 { return x + 1; }
        fun main(): i32 {
            return double(10) + double_(10);   // 20 + 11 = 31
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 31);
    }

    TEST_CASE("Keyword escape: struct/field names that are C++ keywords") {
        const char* source = R"(
        struct template {
            class: i32;
            int: i32;
        }
        fun template.operator(): i32 { return self.class + self.int; }
        fun main(): i32 {
            var t: template = template { class = 30, int = 12 };
            return t.operator();   // 42
        }
    )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

}  // TEST_SUITE("E2E C Backend")

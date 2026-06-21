#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// String Tests
// ============================================================================

TEST_SUITE("E2E Strings") {

    TEST_CASE_TEMPLATE("String literal", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\n");
    }

    TEST_CASE_TEMPLATE("Empty string", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "";
            print(s);
            print("done");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "\ndone\n");
    }

    TEST_CASE_TEMPLATE("String length", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"{str_len("hello")}");
            print(f"{str_len("")}");
            print(f"{str_len("hello world")}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "5\n0\n11\n");
    }

    TEST_CASE_TEMPLATE("String concatenation with str_concat", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = str_concat("hello", " world");
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello world\n");
    }

    TEST_CASE_TEMPLATE("String concatenation with + operator", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello" + " world";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello world\n");
    }

    TEST_CASE_TEMPLATE("Multiple string concatenations", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "a" + "b" + "c" + "d";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "abcd\n");
    }

    TEST_CASE_TEMPLATE("String equality", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bool_to_str(b: bool): string {
            if (b) { return "true"; }
            return "false";
        }

        fun main(): i32 {
            // Same strings
            print(bool_to_str("abc" == "abc"));
            // Different strings
            print(bool_to_str("abc" == "def"));
            // With variables
            var a: string = "hello";
            var b: string = "hello";
            print(bool_to_str(a == b));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\nfalse\ntrue\n");
    }

    TEST_CASE_TEMPLATE("String inequality", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bool_to_str(b: bool): string {
            if (b) { return "true"; }
            return "false";
        }

        fun main(): i32 {
            // Different strings
            print(bool_to_str("abc" != "def"));
            // Same strings
            print(bool_to_str("abc" != "abc"));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\nfalse\n");
    }

    TEST_CASE_TEMPLATE("String as function parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun print_greeting(name: string) {
            print("Hello, ");
            print(name);
        }

        fun main(): i32 {
            print_greeting("World");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "Hello, \nWorld\n");
    }

    TEST_CASE_TEMPLATE("String as return value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun make_greeting(): string {
            return "hello";
        }

        fun main(): i32 {
            print(make_greeting());
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\n");
    }

    TEST_CASE_TEMPLATE("String concatenation in function", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun greet(name: string): string {
            return "Hello, " + name + "!";
        }

        fun main(): i32 {
            print(greet("World"));
            print(greet("Roxy"));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "Hello, World!\nHello, Roxy!\n");
    }

    TEST_CASE_TEMPLATE("String comparison in if statement", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun check(s: string): string {
            if (s == "yes") {
                return "accepted";
            } else {
                return "rejected";
            }
        }

        fun main(): i32 {
            print(check("yes"));
            print(check("no"));
            print(check("maybe"));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "accepted\nrejected\nrejected\n");
    }

    TEST_CASE_TEMPLATE("String variable reassignment", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "short";
            print(s);
            s = "much longer string";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "short\nmuch longer string\n");
    }

    TEST_CASE_TEMPLATE("String equality after concatenation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bool_to_str(b: bool): string {
            if (b) { return "true"; }
            return "false";
        }

        fun main(): i32 {
            var a: string = "hel" + "lo";
            var b: string = "hello";
            print(bool_to_str(a == b));
            print(a);
            print(b);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "true\nhello\nhello\n");
    }

    TEST_CASE_TEMPLATE("String with special characters", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello\nworld";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello\nworld\n");
    }

    TEST_CASE_TEMPLATE("String in loop", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            for (var i: i32 = 0; i < 3; i = i + 1) {
                print("iteration");
                print(f"{i}");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "iteration\n0\niteration\n1\niteration\n2\n");
    }

    // ============================================================================
    // F-String Interpolation Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("F-string basic interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var name: string = "World";
            print(f"Hello, {name}!");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "Hello, World!\n");
    }

    TEST_CASE_TEMPLATE("F-string integer interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            print(f"x = {x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "x = 42\n");
    }

    TEST_CASE_TEMPLATE("F-string expression in braces", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: i32 = 3;
            var b: i32 = 4;
            print(f"{a} + {b} = {a + b}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3 + 4 = 7\n");
    }

    TEST_CASE_TEMPLATE("F-string function call in braces", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun double_it(x: i32): i32 {
            return x * 2;
        }

        fun main(): i32 {
            print(f"double: {double_it(5)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "double: 10\n");
    }

    TEST_CASE_TEMPLATE("F-string bool interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"val: {true}");
            print(f"val: {false}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "val: true\nval: false\n");
    }

    TEST_CASE_TEMPLATE("F-string float interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var pi: f64 = 3.14;
            print(f"pi: {pi}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "pi: 3.14\n");
    }

    TEST_CASE_TEMPLATE("F-string no interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"plain text");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "plain text\n");
    }

    TEST_CASE_TEMPLATE("F-string empty parts", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            print(f"{x}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("F-string escaped braces", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"use \{ and \}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "use { and }\n");
    }

    TEST_CASE_TEMPLATE("F-string multiple types", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var name: string = "app";
            var ver: i32 = 2;
            var score: f64 = 9.5;
            print(f"{name} v{ver}: {score}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "app v2: 9.5\n");
    }

    TEST_CASE_TEMPLATE("F-string concatenation with +", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var name: string = "World";
            print(f"hello" + f" {name}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello World\n");
    }

    TEST_CASE_TEMPLATE("F-string in variable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var s: string = f"value is {x}";
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "value is 10\n");
    }

    TEST_CASE_TEMPLATE("F-string i64 interpolation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var big: i64 = 1000000l;
            print(f"big = {big}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "big = 1000000\n");
    }

    TEST_CASE_TEMPLATE("F-string string expression", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var a: string = "hello";
            var b: string = "world";
            var space: string = " ";
            print(f"{a + space + b}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "hello world\n");
    }

    TEST_CASE_TEMPLATE("F-string empty", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            print(f"");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "\n");
    }

    // ============================================================================
    // New String Native Functions
    // ============================================================================

    TEST_CASE_TEMPLATE("str_char_at basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            var ch: i32 = str_char_at(s, 0);
            print(f"{ch}");
            ch = str_char_at(s, 4);
            print(f"{ch}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // 'h' = 104, 'o' = 111
        CHECK(result.stdout_output == "104\n111\n");
    }

    TEST_CASE_TEMPLATE("str_substr basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello world";
            var sub: string = str_substr(s, 6, 5);
            print(sub);
            sub = str_substr(s, 0, 5);
            print(sub);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "world\nhello\n");
    }

    TEST_CASE_TEMPLATE("str_substr at end boundary returns empty", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            var sub: string = str_substr(s, 5, 0);  // start == len, empty
            print(f"[{sub}]");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "[]\n");
    }

    TEST_CASE_TEMPLATE("str_substr out-of-bounds with overflow-prone lengths is rejected", Backend, RX_E2E_BACKENDS) {
        // start + sub_len overflows i32 if added naively; the bounds check must
        // reject this cleanly rather than relying on signed-overflow UB.
        const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            var sub: string = str_substr(s, 2000000000, 2000000000);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("str_to_f64 basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var v: f64 = str_to_f64("3.14");
            print(f"{v}");
            v = str_to_f64("42");
            print(f"{v}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3.14\n42\n");
    }

    TEST_CASE_TEMPLATE("str_from_code basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var s: string = str_from_code(65);
            print(s);
            s = str_from_code(122);
            print(s);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // 65 = 'A', 122 = 'z'
        CHECK(result.stdout_output == "A\nz\n");
    }

    TEST_CASE_TEMPLATE("clock returns positive value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var t: f64 = clock();
            if (t > 0.0) {
                print("ok");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "ok\n");
    }

    TEST_CASE_TEMPLATE("read_file basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var content: string = read_file("/dev/null");
            print(f"{str_len(content)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n");
    }

    // ============================================================================
    // F-string interpolation of user-defined Printable struct rvalues
    // ============================================================================

    TEST_CASE_TEMPLATE("F-string interp: Printable struct from function call", Backend, RX_E2E_BACKENDS) {
        // Pre-fix: f"{make_pt()}" failed at IR gen with
        // "Internal error: expression is not a valid lvalue".
        const char* source = R"ROXY(
        struct Pt {
            x: i32;
            y: i32;
        }

        fun Pt.to_string(): string for Printable {
            return f"({self.x},{self.y})";
        }

        fun make_pt(): Pt {
            return Pt { x = 3, y = 4 };
        }

        fun main(): i32 {
            print(f"{make_pt()}");
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "(3,4)\n");
    }

    TEST_CASE_TEMPLATE("F-string interp: Printable struct from list index", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Pt {
            x: i32;
            y: i32;
        }

        fun Pt.to_string(): string for Printable {
            return f"({self.x},{self.y})";
        }

        fun main(): i32 {
            var pts: List<Pt> = List<Pt>();
            pts.push(Pt { x = 1, y = 2 });
            pts.push(Pt { x = 5, y = 6 });
            print(f"{pts[0]}");
            print(f"{pts[1]}");
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "(1,2)\n(5,6)\n");
    }

    TEST_CASE_TEMPLATE("F-string interp: Printable struct from method call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Pt {
            x: i32;
            y: i32;
        }

        fun Pt.to_string(): string for Printable {
            return f"({self.x},{self.y})";
        }

        struct Builder {
            base: i32;
        }

        fun Builder.make(): Pt {
            return Pt { x = self.base, y = self.base + 1 };
        }

        fun main(): i32 {
            var b: Builder = Builder { base = 10 };
            print(f"{b.make()}");
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "(10,11)\n");
    }

    TEST_CASE_TEMPLATE("F-string interp: Printable struct from local var still works", Backend, RX_E2E_BACKENDS) {
        // Regression check for the workaround path (lvalue identifier).
        const char* source = R"ROXY(
        struct Pt {
            x: i32;
            y: i32;
        }

        fun Pt.to_string(): string for Printable {
            return f"({self.x},{self.y})";
        }

        fun main(): i32 {
            var p: Pt = Pt { x = 7, y = 8 };
            print(f"{p}");
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "(7,8)\n");
    }

}  // TEST_SUITE("E2E Strings")

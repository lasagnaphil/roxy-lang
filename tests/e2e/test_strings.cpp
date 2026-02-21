#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// String Tests
// ============================================================================

TEST_CASE("E2E - String literal") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\n");
}

TEST_CASE("E2E - Empty string") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "";
            print(s);
            print("done");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "\ndone\n");
}

TEST_CASE("E2E - String length") {
    const char* source = R"(
        fun main(): i32 {
            print(f"{str_len("hello")}");
            print(f"{str_len("")}");
            print(f"{str_len("hello world")}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n0\n11\n");
}

TEST_CASE("E2E - String concatenation with str_concat") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = str_concat("hello", " world");
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - String concatenation with + operator") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello" + " world";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - Multiple string concatenations") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "a" + "b" + "c" + "d";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "abcd\n");
}

TEST_CASE("E2E - String equality") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "true\nfalse\ntrue\n");
}

TEST_CASE("E2E - String inequality") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "true\nfalse\n");
}

TEST_CASE("E2E - String as function parameter") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "Hello, \nWorld\n");
}

TEST_CASE("E2E - String as return value") {
    const char* source = R"(
        fun make_greeting(): string {
            return "hello";
        }

        fun main(): i32 {
            print(make_greeting());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\n");
}

TEST_CASE("E2E - String concatenation in function") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "Hello, World!\nHello, Roxy!\n");
}

TEST_CASE("E2E - String comparison in if statement") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "accepted\nrejected\nrejected\n");
}

TEST_CASE("E2E - String variable reassignment") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "short";
            print(s);
            s = "much longer string";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "short\nmuch longer string\n");
}

TEST_CASE("E2E - String equality after concatenation") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "true\nhello\nhello\n");
}

TEST_CASE("E2E - String with special characters") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello\nworld";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\nworld\n");
}

TEST_CASE("E2E - String in loop") {
    const char* source = R"(
        fun main(): i32 {
            for (var i: i32 = 0; i < 3; i = i + 1) {
                print("iteration");
                print(f"{i}");
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "iteration\n0\niteration\n1\niteration\n2\n");
}

// ============================================================================
// F-String Interpolation Tests
// ============================================================================

TEST_CASE("E2E - F-string basic interpolation") {
    const char* source = R"(
        fun main(): i32 {
            var name: string = "World";
            print(f"Hello, {name}!");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "Hello, World!\n");
}

TEST_CASE("E2E - F-string integer interpolation") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            print(f"x = {x}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "x = 42\n");
}

TEST_CASE("E2E - F-string expression in braces") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 3;
            var b: i32 = 4;
            print(f"{a} + {b} = {a + b}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "3 + 4 = 7\n");
}

TEST_CASE("E2E - F-string function call in braces") {
    const char* source = R"(
        fun double_it(x: i32): i32 {
            return x * 2;
        }

        fun main(): i32 {
            print(f"double: {double_it(5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "double: 10\n");
}

TEST_CASE("E2E - F-string bool interpolation") {
    const char* source = R"(
        fun main(): i32 {
            print(f"val: {true}");
            print(f"val: {false}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "val: true\nval: false\n");
}

TEST_CASE("E2E - F-string float interpolation") {
    const char* source = R"(
        fun main(): i32 {
            var pi: f64 = 3.14;
            print(f"pi: {pi}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "pi: 3.14\n");
}

TEST_CASE("E2E - F-string no interpolation") {
    const char* source = R"(
        fun main(): i32 {
            print(f"plain text");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "plain text\n");
}

TEST_CASE("E2E - F-string empty parts") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 42;
            print(f"{x}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - F-string escaped braces") {
    const char* source = R"(
        fun main(): i32 {
            print(f"use \{ and \}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "use { and }\n");
}

TEST_CASE("E2E - F-string multiple types") {
    const char* source = R"(
        fun main(): i32 {
            var name: string = "app";
            var ver: i32 = 2;
            var score: f64 = 9.5;
            print(f"{name} v{ver}: {score}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "app v2: 9.5\n");
}

TEST_CASE("E2E - F-string concatenation with +") {
    const char* source = R"(
        fun main(): i32 {
            var name: string = "World";
            print(f"hello" + f" {name}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello World\n");
}

TEST_CASE("E2E - F-string in variable") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            var s: string = f"value is {x}";
            print(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "value is 10\n");
}

TEST_CASE("E2E - F-string i64 interpolation") {
    const char* source = R"(
        fun main(): i32 {
            var big: i64 = 1000000l;
            print(f"big = {big}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "big = 1000000\n");
}

TEST_CASE("E2E - F-string string expression") {
    const char* source = R"(
        fun main(): i32 {
            var a: string = "hello";
            var b: string = "world";
            var space: string = " ";
            print(f"{a + space + b}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - F-string empty") {
    const char* source = R"(
        fun main(): i32 {
            print(f"");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "\n");
}

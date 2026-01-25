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
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\n");
}

TEST_CASE("E2E - Empty string") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "";
            print_str(s);
            print_str("done");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "\ndone\n");
}

TEST_CASE("E2E - String length") {
    const char* source = R"(
        fun main(): i32 {
            print(str_len("hello"));
            print(str_len(""));
            print(str_len("hello world"));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "5\n0\n11\n");
}

TEST_CASE("E2E - String concatenation with str_concat") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = str_concat("hello", " world");
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - String concatenation with + operator") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello" + " world";
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "hello world\n");
}

TEST_CASE("E2E - Multiple string concatenations") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "a" + "b" + "c" + "d";
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
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
            print_str(bool_to_str("abc" == "abc"));
            // Different strings
            print_str(bool_to_str("abc" == "def"));
            // With variables
            var a: string = "hello";
            var b: string = "hello";
            print_str(bool_to_str(a == b));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
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
            print_str(bool_to_str("abc" != "def"));
            // Same strings
            print_str(bool_to_str("abc" != "abc"));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "true\nfalse\n");
}

TEST_CASE("E2E - String as function parameter") {
    const char* source = R"(
        fun print_greeting(name: string) {
            print_str("Hello, ");
            print_str(name);
        }

        fun main(): i32 {
            print_greeting("World");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "Hello, \nWorld\n");
}

TEST_CASE("E2E - String as return value") {
    const char* source = R"(
        fun make_greeting(): string {
            return "hello";
        }

        fun main(): i32 {
            print_str(make_greeting());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\n");
}

TEST_CASE("E2E - String concatenation in function") {
    const char* source = R"(
        fun greet(name: string): string {
            return "Hello, " + name + "!";
        }

        fun main(): i32 {
            print_str(greet("World"));
            print_str(greet("Roxy"));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
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
            print_str(check("yes"));
            print_str(check("no"));
            print_str(check("maybe"));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "accepted\nrejected\nrejected\n");
}

TEST_CASE("E2E - String variable reassignment") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "short";
            print_str(s);
            s = "much longer string";
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
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
            print_str(bool_to_str(a == b));
            print_str(a);
            print_str(b);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "true\nhello\nhello\n");
}

TEST_CASE("E2E - String with special characters") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello\nworld";
            print_str(s);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "hello\nworld\n");
}

TEST_CASE("E2E - Mixed print and print_str") {
    const char* source = R"(
        fun main(): i32 {
            print(42);
            print_str("hello");
            print(123);
            print_str("world");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "42\nhello\n123\nworld\n");
}

TEST_CASE("E2E - String in loop") {
    const char* source = R"(
        fun main(): i32 {
            for (var i: i32 = 0; i < 3; i = i + 1) {
                print_str("iteration");
                print(i);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "iteration\n0\niteration\n1\niteration\n2\n");
}

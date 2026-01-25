#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// String Tests
// ============================================================================

TEST_CASE("E2E - String literal and length") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 5);
}

TEST_CASE("E2E - Empty string") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 0);
}

TEST_CASE("E2E - String concatenation with str_concat") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = str_concat("hello", " world");
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 11);  // "hello world" = 11 chars
}

TEST_CASE("E2E - String concatenation with + operator") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello" + " world";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 11);  // "hello world" = 11 chars
}

TEST_CASE("E2E - Multiple string concatenations") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "a" + "b" + "c" + "d";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 4);  // "abcd" = 4 chars
}

TEST_CASE("E2E - String equality - same strings") {
    const char* source = R"(
        fun main(): bool {
            return "abc" == "abc";
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    // VM returns booleans as integers (0 or 1) due to untyped registers
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // true
}

TEST_CASE("E2E - String equality - different strings") {
    const char* source = R"(
        fun main(): bool {
            return "abc" == "def";
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 0);  // false
}

TEST_CASE("E2E - String equality with variables") {
    const char* source = R"(
        fun main(): bool {
            var a: string = "hello";
            var b: string = "hello";
            return a == b;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // true
}

TEST_CASE("E2E - String inequality") {
    const char* source = R"(
        fun main(): bool {
            return "abc" != "def";
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // true
}

TEST_CASE("E2E - String inequality - same strings") {
    const char* source = R"(
        fun main(): bool {
            return "abc" != "abc";
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 0);  // false
}

TEST_CASE("E2E - String as function parameter") {
    const char* source = R"(
        fun get_len(s: string): i32 {
            return str_len(s);
        }
        fun main(): i32 {
            return get_len("test");
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 4);
}

TEST_CASE("E2E - String as return value") {
    const char* source = R"(
        fun make_greeting(): string {
            return "hello";
        }
        fun main(): i32 {
            return str_len(make_greeting());
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 5);
}

TEST_CASE("E2E - String concatenation in function") {
    const char* source = R"(
        fun greet(name: string): string {
            return "Hello, " + name + "!";
        }
        fun main(): i32 {
            return str_len(greet("World"));
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 13);  // "Hello, World!" = 13 chars
}

TEST_CASE("E2E - String comparison in if statement") {
    const char* source = R"(
        fun check(s: string): i32 {
            if (s == "yes") {
                return 1;
            } else {
                return 0;
            }
        }
        fun main(): i32 {
            return check("yes") + check("no") * 10;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // check("yes") = 1, check("no") = 0, so 1 + 0*10 = 1
}

TEST_CASE("E2E - String variable reassignment") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "short";
            s = "much longer string";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 18);  // "much longer string" = 18 chars
}

TEST_CASE("E2E - String equality after concatenation") {
    const char* source = R"(
        fun main(): bool {
            var a: string = "hel" + "lo";
            var b: string = "hello";
            return a == b;
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // true
}

TEST_CASE("E2E - print_str function with output capture") {
    const char* source = R"(
        fun main(): i32 {
            print_str("Hello, World!");
            return 42;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.value == 42);
    CHECK(result.stdout_output == "Hello, World!\n");
}

TEST_CASE("E2E - String with special characters") {
    const char* source = R"(
        fun main(): i32 {
            var s: string = "hello\nworld";
            return str_len(s);
        }
    )";

    Value result = compile_and_run(source, StringView("main"));
    CHECK(result.is_int());
    CHECK(result.as_int == 11);  // "hello\nworld" = 11 chars (including newline)
}

TEST_CASE("E2E - Multiple print_str calls with output capture") {
    const char* source = R"(
        fun main(): i32 {
            print_str("Line 1");
            print_str("Line 2");
            print_str("Line 3");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "Line 1\nLine 2\nLine 3\n");
}

TEST_CASE("E2E - Mixed print and print_str with output capture") {
    const char* source = R"(
        fun main(): i32 {
            print(42);
            print_str("hello");
            print(123);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, StringView("main"));
    CHECK(result.success);
    CHECK(result.stdout_output == "42\nhello\n123\n");
}

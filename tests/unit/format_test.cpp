#include "roxy/core/doctest/doctest.h"
#include "roxy/core/format.hpp"

using namespace rx;

TEST_CASE("format_to - no arguments") {
    char buf[64];
    i32 n = format_to(buf, sizeof(buf), "hello world");
    CHECK(n == 11);
    CHECK(strcmp(buf, "hello world") == 0);
}

TEST_CASE("format_to - integers") {
    char buf[64];

    format_to(buf, sizeof(buf), "{}", 42);
    CHECK(strcmp(buf, "42") == 0);

    format_to(buf, sizeof(buf), "{}", -123);
    CHECK(strcmp(buf, "-123") == 0);

    format_to(buf, sizeof(buf), "{}", (u32)4000000000u);
    CHECK(strcmp(buf, "4000000000") == 0);

    format_to(buf, sizeof(buf), "{}", (i64)9223372036854775807LL);
    CHECK(strcmp(buf, "9223372036854775807") == 0);

    format_to(buf, sizeof(buf), "{}", (u64)18446744073709551615ULL);
    CHECK(strcmp(buf, "18446744073709551615") == 0);
}

TEST_CASE("format_to - bool") {
    char buf[16];

    format_to(buf, sizeof(buf), "{}", true);
    CHECK(strcmp(buf, "true") == 0);

    format_to(buf, sizeof(buf), "{}", false);
    CHECK(strcmp(buf, "false") == 0);
}

TEST_CASE("format_to - char") {
    char buf[16];
    format_to(buf, sizeof(buf), "{}", 'A');
    CHECK(strcmp(buf, "A") == 0);
}

TEST_CASE("format_to - floats") {
    char buf[64];

    format_to(buf, sizeof(buf), "{}", 3.14f);
    CHECK(strcmp(buf, "3.14") == 0);

    format_to(buf, sizeof(buf), "{}", 2.718281828);
    CHECK(strcmp(buf, "2.71828") == 0);
}

TEST_CASE("format_to - strings") {
    char buf[64];

    // String literal
    format_to(buf, sizeof(buf), "hello {}", "world");
    CHECK(strcmp(buf, "hello world") == 0);

    // const char*
    const char* s = "test";
    format_to(buf, sizeof(buf), "{}", s);
    CHECK(strcmp(buf, "test") == 0);

    // null const char*
    const char* null_s = nullptr;
    format_to(buf, sizeof(buf), "{}", null_s);
    CHECK(strcmp(buf, "(null)") == 0);

    // StringView
    StringView sv("hello", 5);
    format_to(buf, sizeof(buf), "{}", sv);
    CHECK(strcmp(buf, "hello") == 0);

    // StringView (not null-terminated)
    StringView sv2("hello world", 5);
    format_to(buf, sizeof(buf), "[{}]", sv2);
    CHECK(strcmp(buf, "[hello]") == 0);
}

TEST_CASE("format_to - multiple arguments") {
    char buf[128];

    format_to(buf, sizeof(buf), "{} + {} = {}", 1, 2, 3);
    CHECK(strcmp(buf, "1 + 2 = 3") == 0);

    format_to(buf, sizeof(buf), "name={} age={} ok={}", "alice", 30, true);
    CHECK(strcmp(buf, "name=alice age=30 ok=true") == 0);
}

TEST_CASE("format_to - escaped braces") {
    char buf[64];

    format_to(buf, sizeof(buf), "{{}}");
    CHECK(strcmp(buf, "{}") == 0);

    format_to(buf, sizeof(buf), "{{{}}}", 42);
    CHECK(strcmp(buf, "{42}") == 0);
}

TEST_CASE("format_to - truncation") {
    char buf[8];

    // "hello world" is 11 chars, buf is 8, should truncate to "hello w\0"
    i32 n = format_to(buf, sizeof(buf), "hello world");
    CHECK(n == 11);              // would-have-written count
    CHECK(strcmp(buf, "hello w") == 0);  // truncated but null-terminated
    CHECK(buf[7] == '\0');
}

TEST_CASE("format_to - size 1 buffer") {
    char buf[1];
    i32 n = format_to(buf, sizeof(buf), "hello");
    CHECK(n == 5);
    CHECK(buf[0] == '\0');
}

TEST_CASE("format_to - size 0 buffer") {
    char buf[1] = {'X'};
    i32 n = format_to(buf, 0, "hello");
    CHECK(n == 0);
    CHECK(buf[0] == 'X');  // untouched
}

TEST_CASE("format_to - enum") {
    enum Color { Red = 0, Green = 1, Blue = 2 };
    char buf[16];
    format_to(buf, sizeof(buf), "{}", Green);
    CHECK(strcmp(buf, "1") == 0);
}

TEST_CASE("format_to - mixed types") {
    char buf[128];
    StringView name("foo", 3);
    format_to(buf, sizeof(buf), "error at line {}: '{}' is not a {}", (u32)42, name, "number");
    CHECK(strcmp(buf, "error at line 42: 'foo' is not a number") == 0);
}

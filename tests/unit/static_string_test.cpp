#include "roxy/core/doctest/doctest.h"
#include "roxy/core/static_string.hpp"

using namespace rx;

TEST_CASE("StaticString - default construction") {
    StaticString<32> s;
    CHECK(s.size() == 0);
    CHECK(s.empty());
    CHECK(s.data()[0] == '\0');
    CHECK(strcmp(s.c_str(), "") == 0);
}

TEST_CASE("StaticString - construct from const char*") {
    StaticString<32> s("hello");
    CHECK(s.size() == 5);
    CHECK(!s.empty());
    CHECK(strcmp(s.c_str(), "hello") == 0);
}

TEST_CASE("StaticString - construct from const char* with length") {
    StaticString<32> s("hello world", 5);
    CHECK(s.size() == 5);
    CHECK(strcmp(s.c_str(), "hello") == 0);
}

TEST_CASE("StaticString - construct from StringView") {
    StringView sv("hello", 5);
    StaticString<32> s(sv);
    CHECK(s.size() == 5);
    CHECK(strcmp(s.c_str(), "hello") == 0);
}

TEST_CASE("StaticString - push_back") {
    StaticString<8> s;
    s.push_back('a');
    s.push_back('b');
    s.push_back('c');
    CHECK(s.size() == 3);
    CHECK(strcmp(s.c_str(), "abc") == 0);
}

TEST_CASE("StaticString - push_back to capacity") {
    StaticString<4> s;
    s.push_back('a');
    s.push_back('b');
    s.push_back('c');
    CHECK(s.size() == 3);
    CHECK(s.size() == s.capacity());
    CHECK(strcmp(s.c_str(), "abc") == 0);
}

TEST_CASE("StaticString - append") {
    StaticString<32> s("hello");
    s.append(" world", 6);
    CHECK(s.size() == 11);
    CHECK(strcmp(s.c_str(), "hello world") == 0);
}

TEST_CASE("StaticString - append StringView") {
    StaticString<32> s("hello");
    s.append(StringView(" world", 6));
    CHECK(s.size() == 11);
    CHECK(strcmp(s.c_str(), "hello world") == 0);
}

TEST_CASE("StaticString - clear") {
    StaticString<32> s("hello");
    s.clear();
    CHECK(s.size() == 0);
    CHECK(s.empty());
    CHECK(strcmp(s.c_str(), "") == 0);
}

TEST_CASE("StaticString - operator[]") {
    StaticString<32> s("hello");
    CHECK(s[0] == 'h');
    CHECK(s[4] == 'o');
    s[0] = 'H';
    CHECK(s[0] == 'H');
}

TEST_CASE("StaticString - front and back") {
    StaticString<32> s("hello");
    CHECK(s.front() == 'h');
    CHECK(s.back() == 'o');
}

TEST_CASE("StaticString - comparison with StringView") {
    StaticString<32> s("hello");
    CHECK(s == StringView("hello", 5));
    CHECK(s != StringView("world", 5));
}

TEST_CASE("StaticString - comparison with const char*") {
    StaticString<32> s("hello");
    CHECK(s == "hello");
    CHECK(s != "world");
}

TEST_CASE("StaticString - comparison between StaticStrings") {
    StaticString<32> a("hello");
    StaticString<16> b("hello");
    StaticString<32> c("world");
    CHECK(a == b);
    CHECK(a != c);
}

TEST_CASE("StaticString - implicit StringView conversion") {
    StaticString<32> s("hello");
    StringView sv = s;
    CHECK(sv.size() == 5);
    CHECK(sv == "hello");
}

TEST_CASE("StaticString - iterators / range-for") {
    StaticString<32> s("abc");
    rx::String result;
    for (char c : s) {
        result.push_back(c);
    }
    CHECK(result == "abc");
}

TEST_CASE("StaticString - format_to overload") {
    StaticString<32> s;
    format_to(s, "hello {}", "world");
    CHECK(s == "hello world");
    CHECK(s.size() == 11);
}

TEST_CASE("StaticString - format method") {
    StaticString<32> s;
    s.format("x={}, y={}", 10, 20);
    CHECK(s == "x=10, y=20");

    // format() resets the string
    s.format("new");
    CHECK(s == "new");
    CHECK(s.size() == 3);
}

TEST_CASE("StaticString - format as argument") {
    StaticString<32> name("world");
    char buf[64];
    format_to(buf, sizeof(buf), "hello {}", name);
    CHECK(strcmp(buf, "hello world") == 0);
}

TEST_CASE("StaticString - capacity") {
    StaticString<16> s;
    CHECK(s.capacity() == 15);

    StaticString<32> s2;
    CHECK(s2.capacity() == 31);
}

TEST_CASE("StaticString - set_size") {
    StaticString<32> s("hello world");
    s.set_size(5);
    CHECK(s.size() == 5);
    CHECK(strcmp(s.c_str(), "hello") == 0);
}

TEST_CASE("StaticString - copy") {
    StaticString<32> a("hello");
    StaticString<32> b = a;
    CHECK(b == "hello");
    b.push_back('!');
    CHECK(b == "hello!");
    CHECK(a == "hello"); // original unchanged
}

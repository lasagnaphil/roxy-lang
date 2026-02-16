#include "roxy/core/doctest/doctest.h"
#include "roxy/core/string.hpp"
#include "roxy/core/format.hpp"

using namespace rx;

TEST_CASE("String - sizeof") {
    CHECK(sizeof(String) == 24);
}

TEST_CASE("String - SSO basics") {
    SUBCASE("empty string") {
        String s;
        CHECK(s.empty());
        CHECK(s.size() == 0);
        CHECK(s.data()[0] == '\0');
        CHECK(s == "");
    }

    SUBCASE("short string") {
        String s("hello");
        CHECK(s.size() == 5);
        CHECK(s == "hello");
        CHECK(!s.empty());
    }

    SUBCASE("exactly 22 chars (max SSO)") {
        String s("1234567890123456789012");
        CHECK(s.size() == 22);
        CHECK(s == "1234567890123456789012");
    }

    SUBCASE("23 chars - heap") {
        String s("12345678901234567890123");
        CHECK(s.size() == 23);
        CHECK(s == "12345678901234567890123");
    }

    SUBCASE("from nullptr") {
        String s(static_cast<const char*>(nullptr));
        CHECK(s.empty());
        CHECK(s.size() == 0);
    }
}

TEST_CASE("String - construct from StringView") {
    StringView sv("hello world", 5);
    String s(sv);
    CHECK(s.size() == 5);
    CHECK(s == "hello");
}

TEST_CASE("String - copy semantics") {
    SUBCASE("copy SSO") {
        String a("hello");
        String b(a);
        CHECK(b == "hello");
        CHECK(a == "hello");
        // independent
        b.push_back('!');
        CHECK(b == "hello!");
        CHECK(a == "hello");
    }

    SUBCASE("copy heap") {
        String a("this is a long string that exceeds SSO");
        String b(a);
        CHECK(b == a);
        // independent
        b.push_back('!');
        CHECK(a == "this is a long string that exceeds SSO");
    }
}

TEST_CASE("String - move semantics") {
    SUBCASE("move SSO") {
        String a("hello");
        String b(static_cast<String&&>(a));
        CHECK(b == "hello");
        CHECK(a.empty());
    }

    SUBCASE("move heap") {
        String a("this is a long string that exceeds SSO capacity");
        const char* ptr = a.data();
        String b(static_cast<String&&>(a));
        CHECK(b == "this is a long string that exceeds SSO capacity");
        CHECK(b.data() == ptr);  // no reallocation
        CHECK(a.empty());
    }
}

TEST_CASE("String - assignment") {
    SUBCASE("SSO to SSO") {
        String a("hello");
        String b("world");
        b = a;
        CHECK(b == "hello");
    }

    SUBCASE("SSO to heap") {
        String a("hello");
        String b("this is a long string that exceeds SSO capacity");
        b = a;
        CHECK(b == "hello");
    }

    SUBCASE("heap to SSO") {
        String a("this is a long string that exceeds SSO capacity");
        String b("short");
        b = a;
        CHECK(b == "this is a long string that exceeds SSO capacity");
    }

    SUBCASE("self-assignment") {
        String a("hello");
        a = a;
        CHECK(a == "hello");
    }

    SUBCASE("from const char*") {
        String s;
        s = "hello";
        CHECK(s == "hello");
    }

    SUBCASE("from StringView") {
        String s;
        s = StringView("world", 5);
        CHECK(s == "world");
    }
}

TEST_CASE("String - push_back and append") {
    SUBCASE("push_back within SSO") {
        String s;
        for (int i = 0; i < 22; i++) {
            s.push_back('a');
        }
        CHECK(s.size() == 22);
    }

    SUBCASE("push_back SSO to heap transition") {
        String s;
        for (int i = 0; i < 23; i++) {
            s.push_back('b');
        }
        CHECK(s.size() == 23);
        CHECK(s == "bbbbbbbbbbbbbbbbbbbbbbb");
    }

    SUBCASE("append within SSO") {
        String s("hello");
        s.append(" world", 6);
        CHECK(s == "hello world");
    }

    SUBCASE("append causing SSO to heap") {
        String s("12345678901234567890");  // 20 chars
        s.append("abcde", 5);
        CHECK(s.size() == 25);
        CHECK(s == "12345678901234567890abcde");
    }

    SUBCASE("append StringView") {
        String s("hello");
        StringView sv(" world");
        s.append(sv);
        CHECK(s == "hello world");
    }
}

TEST_CASE("String - clear") {
    String s("hello");
    s.clear();
    CHECK(s.empty());
    CHECK(s.size() == 0);

    String long_s("this is a long string that exceeds SSO capacity");
    long_s.clear();
    CHECK(long_s.empty());
}

TEST_CASE("String - reserve") {
    String s;
    s.reserve(100);
    CHECK(s.capacity() >= 100);
    CHECK(s.empty());

    s.append("hello", 5);
    CHECK(s == "hello");
    CHECK(s.capacity() >= 100);
}

TEST_CASE("String - resize") {
    SUBCASE("grow with fill") {
        String s("hi");
        s.resize(5, 'x');
        CHECK(s.size() == 5);
        CHECK(s == "hixxx");
    }

    SUBCASE("shrink") {
        String s("hello world");
        s.resize(5);
        CHECK(s.size() == 5);
        CHECK(s == "hello");
    }

    SUBCASE("shrink heap to SSO") {
        String s("this is a long string that exceeds SSO capacity");
        s.resize(3);
        CHECK(s.size() == 3);
        CHECK(s == "thi");
    }
}

TEST_CASE("String - comparison operators") {
    String a("hello");
    String b("hello");
    String c("world");

    SUBCASE("String == String") {
        CHECK(a == b);
        CHECK(a != c);
    }

    SUBCASE("String == const char*") {
        CHECK(a == "hello");
        CHECK(a != "world");
    }

    SUBCASE("const char* == String") {
        CHECK("hello" == a);
        CHECK("world" != a);
    }

    SUBCASE("String == StringView") {
        StringView sv("hello");
        CHECK(a == sv);
        CHECK(sv == a);
    }
}

TEST_CASE("String - find char") {
    String s("hello world");

    CHECK(s.find('h') == 0);
    CHECK(s.find('o') == 4);
    CHECK(s.find('o', 5) == 7);
    CHECK(s.find('z') == String::npos);
}

TEST_CASE("String - find const char*") {
    String s("hello world hello");

    CHECK(s.find("hello") == 0);
    CHECK(s.find("world") == 6);
    CHECK(s.find("hello", 1) == 12);
    CHECK(s.find("xyz") == String::npos);
    CHECK(s.find("") == 0);
}

TEST_CASE("String - substr") {
    String s("hello world");

    CHECK(s.substr(0, 5) == "hello");
    CHECK(s.substr(6) == "world");
    CHECK(s.substr(6, 5) == "world");
    CHECK(s.substr(0) == "hello world");
}

TEST_CASE("String - StringView conversion") {
    String s("hello");
    StringView sv = s;
    CHECK(sv.size() == 5);
    CHECK(sv == "hello");
}

TEST_CASE("String - iterators") {
    String s("abc");
    u32 count = 0;
    for (char c : s) {
        (void)c;
        count++;
    }
    CHECK(count == 3);
}

TEST_CASE("String - front/back/operator[]") {
    String s("abc");
    CHECK(s.front() == 'a');
    CHECK(s.back() == 'c');
    CHECK(s[1] == 'b');
}

TEST_CASE("format() returning String") {
    SUBCASE("short format") {
        String s = format("hello {}", "world");
        CHECK(s == "hello world");
    }

    SUBCASE("with integers") {
        String s = format("v{}", 42);
        CHECK(s == "v42");
    }

    SUBCASE("with String arg") {
        String name("world");
        String s = format("hello {}", name);
        CHECK(s == "hello world");
    }

    SUBCASE("long format exceeding stack buffer") {
        // Create a format result > 256 chars
        char long_str[300];
        memset(long_str, 'x', 299);
        long_str[299] = '\0';
        String s = format("{}", long_str);
        CHECK(s.size() == 299);
    }
}

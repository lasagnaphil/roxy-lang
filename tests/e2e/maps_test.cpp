#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Map Tests
// ============================================================================

TEST_CASE("E2E - Map basic insert and get") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            m.insert(3, 30);
            print(f"{m.get(1)}");
            print(f"{m.get(2)}");
            print(f"{m.get(3)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n30\n");
}

TEST_CASE("E2E - Map len tracking") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            print(f"{m.len()}");
            m.insert(1, 10);
            print(f"{m.len()}");
            m.insert(2, 20);
            print(f"{m.len()}");
            m.insert(3, 30);
            print(f"{m.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n1\n2\n3\n");
}

TEST_CASE("E2E - Map index operator read and write") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m[1] = 100;
            m[2] = 200;
            print(f"{m[1]}");
            print(f"{m[2]}");
            // Overwrite via indexing
            m[1] = 999;
            print(f"{m[1]}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n200\n999\n");
}

TEST_CASE("E2E - Map contains") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(42, 1);
            if (m.contains(42)) {
                print("yes");
            }
            if (!m.contains(99)) {
                print("no");
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "yes\nno\n");
}

TEST_CASE("E2E - Map remove") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            var removed: bool = m.remove(1);
            print(f"{removed}");
            print(f"{m.len()}");
            if (!m.contains(1)) {
                print("gone");
            }
            // Remove non-existent key
            var not_removed: bool = m.remove(99);
            print(f"{not_removed}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "2\ntrue\n1\ngone\nfalse\n");
}

TEST_CASE("E2E - Map clear") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            m.clear();
            print(f"{m.len()}");
            if (!m.contains(1)) {
                print("empty");
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "2\n0\nempty\n");
}

TEST_CASE("E2E - Map string keys") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            var key1: string = "hello";
            var key2: string = "world";
            m.insert(key1, 1);
            m.insert(key2, 2);
            print(f"{m.get(key1)}");
            print(f"{m.get(key2)}");
            print(f"{m.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n2\n");
}

TEST_CASE("E2E - Map i32 keys with string values") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, string> = Map<i32, string>();
            m.insert(1, "one");
            m.insert(2, "two");
            print(m.get(1));
            print(m.get(2));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "one\ntwo\n");
}

TEST_CASE("E2E - Map overwrite existing key") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            print(f"{m.get(1)}");
            m.insert(1, 99);
            print(f"{m.get(1)}");
            print(f"{m.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n99\n1\n");
}

TEST_CASE("E2E - Map growth and rehashing") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            var i: i32 = 0;
            while (i < 50) {
                m.insert(i, i * 10);
                i = i + 1;
            }
            print(f"{m.len()}");
            print(f"{m.get(0)}");
            print(f"{m.get(25)}");
            print(f"{m.get(49)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "50\n0\n250\n490\n");
}

TEST_CASE("E2E - Map with initial capacity") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>(64);
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            print(f"{m.get(1)}");
            print(f"{m.get(2)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "2\n10\n20\n");
}

TEST_CASE("E2E - Map missing key runtime error") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            var x: i32 = m.get(99);
            return x;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK_FALSE(result.success);
}

TEST_CASE("E2E - Map keys and values") {
    const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(10, 100);
            m.insert(20, 200);
            var k: List<i32> = m.keys();
            var v: List<i32> = m.values();
            print(f"{k.len()}");
            print(f"{v.len()}");
            var key_sum: i32 = k[0] + k[1];
            var val_sum: i32 = v[0] + v[1];
            print(f"{key_sum}");
            print(f"{val_sum}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "2\n2\n30\n300\n");
}

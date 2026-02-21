#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// List Tests
// ============================================================================

TEST_CASE("E2E - List basic operations") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            print(f"{lst[0]}");
            print(f"{lst[1]}");
            print(f"{lst[2]}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n30\n");
}

TEST_CASE("E2E - List length and capacity") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            print(f"{lst.len()}");
            lst.push(1);
            lst.push(2);
            lst.push(3);
            print(f"{lst.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n3\n");
}

TEST_CASE("E2E - List with initial capacity") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>(10);
            print(f"{lst.len()}");
            print(f"{lst.cap()}");
            lst.push(42);
            print(f"{lst.len()}");
            print(f"{lst.cap()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n10\n1\n10\n");
}

TEST_CASE("E2E - List pop") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            var x: i32 = lst.pop();
            print(f"{x}");
            print(f"{lst.len()}");
            var y: i32 = lst.pop();
            print(f"{y}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n2\n20\n1\n");
}

TEST_CASE("E2E - List index assignment") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            lst[0] = 100;
            lst[2] = 300;
            print(f"{lst[0]}");
            print(f"{lst[1]}");
            print(f"{lst[2]}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n20\n300\n");
}

TEST_CASE("E2E - List with loop") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(1);
            lst.push(2);
            lst.push(3);
            lst.push(4);
            lst.push(5);

            var sum: i32 = 0;
            for (var i: i32 = 0; i < lst.len(); i = i + 1) {
                print(f"{lst[i]}");
                sum = sum + lst[i];
            }
            print(f"{sum}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n5\n15\n");
}

TEST_CASE("E2E - List swap") {
    const char* source = R"(
        fun swap(lst: inout List<i32>, i: i32, j: i32) {
            var temp: i32 = lst[i];
            lst[i] = lst[j];
            lst[j] = temp;
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            swap(inout lst, 0, 2);
            print(f"{lst[0]}");
            print(f"{lst[1]}");
            print(f"{lst[2]}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n20\n10\n");
}

TEST_CASE("E2E - List quicksort") {
    const char* source = R"(
        fun swap(lst: inout List<i32>, i: i32, j: i32) {
            var temp: i32 = lst[i];
            lst[i] = lst[j];
            lst[j] = temp;
        }

        fun partition(lst: inout List<i32>, low: i32, high: i32): i32 {
            var pivot: i32 = lst[high];
            var i: i32 = low - 1;
            for (var j: i32 = low; j < high; j = j + 1) {
                if (lst[j] <= pivot) {
                    i = i + 1;
                    swap(inout lst, i, j);
                }
            }
            swap(inout lst, i + 1, high);
            return i + 1;
        }

        fun quicksort(lst: inout List<i32>, low: i32, high: i32) {
            if (low < high) {
                var pi: i32 = partition(inout lst, low, high);
                quicksort(inout lst, low, pi - 1);
                quicksort(inout lst, pi + 1, high);
            }
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(5);
            lst.push(2);
            lst.push(8);
            lst.push(1);
            lst.push(9);

            quicksort(inout lst, 0, lst.len() - 1);

            for (var i: i32 = 0; i < lst.len(); i = i + 1) {
                print(f"{lst[i]}");
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n5\n8\n9\n");
}

TEST_CASE("E2E - List growth") {
    const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            for (var i: i32 = 0; i < 20; i = i + 1) {
                lst.push(i * 10);
            }
            print(f"{lst.len()}");
            print(f"{lst[0]}");
            print(f"{lst[9]}");
            print(f"{lst[19]}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n0\n90\n190\n");
}

TEST_CASE("E2E - List sum function") {
    const char* source = R"(
        fun sum(lst: List<i32>): i32 {
            var total: i32 = 0;
            for (var i: i32 = 0; i < lst.len(); i = i + 1) {
                total = total + lst[i];
            }
            return total;
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            print(f"{sum(lst)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "60\n");
}

TEST_CASE("E2E - List value parameter isolation") {
    const char* source = R"(
        fun modify(lst: List<i32>) {
            lst[0] = 999;
            lst.push(40);
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            modify(lst);
            print(f"{lst[0]}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n3\n");
}

TEST_CASE("E2E - List inout parameter mutation") {
    const char* source = R"(
        fun modify(lst: inout List<i32>) {
            lst[0] = 999;
            lst.push(40);
        }

        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(10);
            lst.push(20);
            lst.push(30);
            modify(inout lst);
            print(f"{lst[0]}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "999\n4\n");
}

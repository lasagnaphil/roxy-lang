#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Array Tests
// ============================================================================

TEST_CASE("E2E - Array basic operations") {
    const char* source = R"(
        fun main(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n30\n");
}

TEST_CASE("E2E - Array length") {
    const char* source = R"(
        fun main(): i32 {
            var arr: i32[] = array_new_int(7);
            print(array_len(arr));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "7\n");
}

TEST_CASE("E2E - Array with loop") {
    const char* source = R"(
        fun main(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 1;
            arr[1] = 2;
            arr[2] = 3;
            arr[3] = 4;
            arr[4] = 5;

            var sum: i32 = 0;
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
                sum = sum + arr[i];
            }
            print(sum);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n5\n15\n");
}

TEST_CASE("E2E - Array swap") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun main(): i32 {
            var arr: i32[] = array_new_int(3);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            swap(arr, 0, 2);
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n20\n10\n");
}

TEST_CASE("E2E - Quicksort") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun partition(arr: i32[], low: i32, high: i32): i32 {
            var pivot: i32 = arr[high];
            var i: i32 = low - 1;
            for (var j: i32 = low; j < high; j = j + 1) {
                if (arr[j] <= pivot) {
                    i = i + 1;
                    swap(arr, i, j);
                }
            }
            swap(arr, i + 1, high);
            return i + 1;
        }

        fun quicksort(arr: i32[], low: i32, high: i32) {
            if (low < high) {
                var pi: i32 = partition(arr, low, high);
                quicksort(arr, low, pi - 1);
                quicksort(arr, pi + 1, high);
            }
        }

        fun main(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 5;
            arr[1] = 2;
            arr[2] = 8;
            arr[3] = 1;
            arr[4] = 9;

            quicksort(arr, 0, array_len(arr) - 1);

            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n5\n8\n9\n");
}

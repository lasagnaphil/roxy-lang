#include "roxy/core/doctest/doctest.h"
#include "e2e_test_helpers.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Array Tests
// ============================================================================

TEST_CASE("E2E - Array basic operations") {
    const char* source = R"(
        fun test_array(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return arr[0] + arr[1] + arr[2];
        }
    )";

    Value result = compile_and_run(source, StringView("test_array"));
    CHECK(result.is_int());
    CHECK(result.as_int == 60);  // 10 + 20 + 30
}

TEST_CASE("E2E - Array length") {
    const char* source = R"(
        fun test_len(): i32 {
            var arr: i32[] = array_new_int(7);
            print(array_len(arr));
            return array_len(arr);
        }
    )";

    Value result = compile_and_run(source, StringView("test_len"));
    CHECK(result.is_int());
    CHECK(result.as_int == 7);
}

TEST_CASE("E2E - Array with loop") {
    const char* source = R"(
        fun sum_array(): i32 {
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
            return sum;
        }
    )";

    Value result = compile_and_run(source, StringView("sum_array"));
    CHECK(result.is_int());
    CHECK(result.as_int == 15);  // 1 + 2 + 3 + 4 + 5
}

TEST_CASE("E2E - Array swap") {
    const char* source = R"(
        fun swap(arr: i32[], i: i32, j: i32) {
            var temp: i32 = arr[i];
            arr[i] = arr[j];
            arr[j] = temp;
        }

        fun test_swap(): i32 {
            var arr: i32[] = array_new_int(3);
            arr[0] = 10;
            arr[1] = 20;
            arr[2] = 30;
            swap(arr, 0, 2);
            print(arr[0]);
            print(arr[1]);
            print(arr[2]);
            return arr[0] * 100 + arr[1] * 10 + arr[2];
        }
    )";

    Value result = compile_and_run(source, StringView("test_swap"));
    CHECK(result.is_int());
    CHECK(result.as_int == 3210);  // arr[0]=30, arr[1]=20, arr[2]=10 -> 30*100 + 20*10 + 10 = 3210
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

        fun test_quicksort(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 5;
            arr[1] = 2;
            arr[2] = 8;
            arr[3] = 1;
            arr[4] = 9;

            quicksort(arr, 0, array_len(arr) - 1);

            // Print sorted array
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
            }

            return arr[0];
        }
    )";

    Value result = compile_and_run(source, StringView("test_quicksort"));
    CHECK(result.is_int());
    CHECK(result.as_int == 1);  // First element should be 1 after sorting
}

TEST_CASE("E2E - Quicksort verify all elements") {
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

        fun verify_sorted(): i32 {
            var arr: i32[] = array_new_int(5);
            arr[0] = 5;
            arr[1] = 2;
            arr[2] = 8;
            arr[3] = 1;
            arr[4] = 9;

            quicksort(arr, 0, array_len(arr) - 1);

            // Print sorted array
            for (var i: i32 = 0; i < array_len(arr); i = i + 1) {
                print(arr[i]);
            }

            // Return encoded sorted array for verification
            return arr[0] + arr[1] * 10 + arr[2] * 100 + arr[3] * 1000 + arr[4] * 10000;
        }
    )";

    Value result = compile_and_run(source, StringView("verify_sorted"));
    CHECK(result.is_int());
    CHECK(result.as_int == 98521);  // [1, 2, 5, 8, 9] encoded
}

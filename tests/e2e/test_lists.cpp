#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// List Tests
// ============================================================================

TEST_SUITE("E2E Lists") {

    TEST_CASE_TEMPLATE("List basic operations", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n20\n30\n");
    }

    TEST_CASE_TEMPLATE("List length and capacity", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n3\n");
    }

    TEST_CASE_TEMPLATE("List with initial capacity", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n10\n1\n10\n");
    }

    TEST_CASE_TEMPLATE("List pop", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n2\n20\n1\n");
    }

    TEST_CASE_TEMPLATE("List index assignment", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n20\n300\n");
    }

    TEST_CASE_TEMPLATE("List with loop", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n3\n4\n5\n15\n");
    }

    TEST_CASE_TEMPLATE("List swap", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n20\n10\n");
    }

    TEST_CASE("List quicksort") {  // VM-only: C backend: copyable container (List/Map) value-param deep-copy not emitted (lowering inserts it for the VM path only)  // VM-only: C backend: struct-by-value copy semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n5\n8\n9\n");
    }

    TEST_CASE_TEMPLATE("List growth", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "20\n0\n90\n190\n");
    }

    TEST_CASE_TEMPLATE("List sum function", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "60\n");
    }

    TEST_CASE("List value parameter isolation") {  // VM-only: C backend: copyable container (List/Map) value-param deep-copy not emitted (lowering inserts it for the VM path only)  // VM-only: C backend: struct-by-value copy semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n3\n");
    }

    TEST_CASE_TEMPLATE("List inout parameter mutation", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "999\n4\n");
    }

    // ============================================================================
    // List with struct element tests (multi-slot)
    // ============================================================================

    TEST_CASE_TEMPLATE("List of 2-slot struct (Point)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var lst: List<Point> = List<Point>();
            lst.push(Point { x = 10, y = 20 });
            lst.push(Point { x = 30, y = 40 });
            lst.push(Point { x = 50, y = 60 });
            print(f"{lst[0].x} {lst[0].y}");
            print(f"{lst[1].x} {lst[1].y}");
            print(f"{lst[2].x} {lst[2].y}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10 20\n30 40\n50 60\n3\n");
    }

    TEST_CASE_TEMPLATE("List of 3-slot struct (Vec3)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Vec3 {
            x: f32;
            y: f32;
            z: f32;
        }

        fun main(): i32 {
            var lst: List<Vec3> = List<Vec3>();
            lst.push(Vec3 { x = 1.0f, y = 2.0f, z = 3.0f });
            lst.push(Vec3 { x = 4.0f, y = 5.0f, z = 6.0f });
            var v: Vec3 = lst[0];
            print(f"{v.x} {v.y} {v.z}");
            var w: Vec3 = lst[1];
            print(f"{w.x} {w.y} {w.z}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1 2 3\n4 5 6\n");
    }

    TEST_CASE_TEMPLATE("List of large struct (5 slots)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct BigStruct {
            a: i32;
            b: i32;
            c: i32;
            d: i32;
            e: i32;
        }

        fun main(): i32 {
            var lst: List<BigStruct> = List<BigStruct>();
            lst.push(BigStruct { a = 1, b = 2, c = 3, d = 4, e = 5 });
            lst.push(BigStruct { a = 10, b = 20, c = 30, d = 40, e = 50 });
            var s: BigStruct = lst[0];
            print(f"{s.a} {s.b} {s.c} {s.d} {s.e}");
            var t: BigStruct = lst[1];
            print(f"{t.a} {t.b} {t.c} {t.d} {t.e}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1 2 3 4 5\n10 20 30 40 50\n2\n");
    }

    TEST_CASE_TEMPLATE("List of struct index set", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var lst: List<Point> = List<Point>();
            lst.push(Point { x = 1, y = 2 });
            lst.push(Point { x = 3, y = 4 });
            lst[0] = Point { x = 100, y = 200 };
            print(f"{lst[0].x} {lst[0].y}");
            print(f"{lst[1].x} {lst[1].y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100 200\n3 4\n");
    }

    TEST_CASE_TEMPLATE("List of struct pop", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Vec3 {
            x: f32;
            y: f32;
            z: f32;
        }

        fun main(): i32 {
            var lst: List<Vec3> = List<Vec3>();
            lst.push(Vec3 { x = 1.0f, y = 2.0f, z = 3.0f });
            lst.push(Vec3 { x = 4.0f, y = 5.0f, z = 6.0f });
            var v: Vec3 = lst.pop();
            print(f"{v.x} {v.y} {v.z}");
            print(f"{lst.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "4 5 6\n1\n");
    }

    TEST_CASE_TEMPLATE("List of struct loop iteration", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var lst: List<Point> = List<Point>();
            for (var i: i32 = 0; i < 5; i = i + 1) {
                lst.push(Point { x = i, y = i * 10 });
            }
            var sum: i32 = 0;
            for (var i: i32 = 0; i < lst.len(); i = i + 1) {
                sum = sum + lst[i].x + lst[i].y;
            }
            print(f"{sum}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "110\n");
    }

    // ============================================================================
    // Sign-extension of 1-slot integer elements (regression: INDEX_GET_LIST on an
    // inline 1-slot element zero-extended to 64 bits, so negative i32s compared
    // as a large positive 32-bit number despite printing correctly).
    // ============================================================================

    TEST_CASE_TEMPLATE("List<i32>: negative element compares as negative", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var lst: List<i32> = List<i32>();
            lst.push(-1);
            var v: i32 = lst[0];
            if (v == -1 && v < 0) {
                return 42;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("List<Struct>: negative i32 field of struct element compares as negative", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct E { enc: i32; }
        fun main(): i32 {
            var lst: List<E> = List<E>();
            lst.push(E { enc = -1 });
            var v: i32 = lst[0].enc;
            if (v == -1 && v < 0) {
                return 42;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("List<i32>: while loop with negative-sentinel guard terminates", Backend, RX_E2E_BACKENDS) {
        // The TODO note's "while-loop-doesn't-re-check-condition" symptom:
        // assigning `idx = lst[idx].enc` where enc=-1 would silently give a large
        // positive number under zero-extension, so `idx >= 0` stayed true and the
        // loop re-entered with an out-of-bounds index. Should terminate cleanly now.
        const char* source = R"(
        struct Env { enc: i32; }
        fun main(): i32 {
            var envs: List<Env> = List<Env>();
            envs.push(Env { enc = -1 });
            envs.push(Env { enc = 0 });   // parent link at 0 points to first
            envs.push(Env { enc = 1 });   // leaf points to parent

            var idx: i32 = 2;
            var hops: i32 = 0;
            while (idx >= 0) {
                idx = envs[idx].enc;
                hops = hops + 1;
            }
            return hops;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    // ============================================================================
    // Element cleanup for noncopyable value-struct elements (regression).
    // List element cleanup must pass the in-place element address to delete_value
    // for value structs (free_obj == false), not load the first two slots as a
    // pointer. Older code always loaded a pointer, corrupting cleanup of
    // List<value-struct-with-owned-fields>.
    // ============================================================================

    TEST_CASE_TEMPLATE("List<value-struct with destructor>: per-element cleanup", Backend, RX_E2E_BACKENDS) {
        // Each Item is an inline value struct with a user destructor. When the list
        // is destroyed at scope exit, each element's destructor must run with the
        // correct `self`, in element order.
        const char* source = R"(
        struct Item { id: i32; }
        fun delete Item() { print(f"del {self.id}"); }

        fun main(): i32 {
            var lst: List<Item> = List<Item>();
            lst.push(Item { id = 1 });
            lst.push(Item { id = 2 });
            lst.push(Item { id = 3 });
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "del 1\ndel 2\ndel 3\n");
    }

    TEST_CASE_TEMPLATE("List<struct with uniq field>: per-element field cleanup", Backend, RX_E2E_BACKENDS) {
        // Holder is an inline value struct owning a uniq Inner. Destroying the list
        // must walk each element's fields in place and free the owned Inner.
        const char* source = R"(
        struct Inner { tag: i32; }
        fun delete Inner() { print(f"inner {self.tag}"); }
        struct Holder { val: uniq Inner; }

        fun main(): i32 {
            var lst: List<Holder> = List<Holder>();
            lst.push(Holder { val = uniq Inner { tag = 10 } });
            lst.push(Holder { val = uniq Inner { tag = 20 } });
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "inner 10\ninner 20\n");
    }

}  // TEST_SUITE("E2E Lists")

#include "roxy/core/doctest/doctest.h"
#include "test_e2e_backend.hpp"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Index-operator exceptions: an out-of-bounds `list[i]` throws IndexError and a
// missing-key `m[k]` throws KeyError — both catchable typed exceptions.
// ============================================================================

TEST_SUITE("E2E Index Exceptions") {

    TEST_CASE_TEMPLATE("list[i] out of bounds throws IndexError", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(10);
            xs.push(20);
            try {
                var v: i32 = xs[5];
                print("unreachable");
            } catch (e: IndexError) {
                print(f"caught: {e.message()}");
            } finally {
                print("done");
            }
            print(f"in bounds: {xs[1]}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught: List index out of bounds\ndone\nin bounds: 20\n");
    }

    TEST_CASE_TEMPLATE("negative list index throws IndexError", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(7);
            try {
                var v: i32 = xs[-1];
                print("unreachable");
            } catch (e: IndexError) {
                print("caught negative");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught negative\n");
    }

    TEST_CASE_TEMPLATE("m[k] missing key throws KeyError", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            try {
                var v: i32 = m[99];
                print("unreachable");
            } catch (e: KeyError) {
                print(f"caught: {e.message()}");
            }
            print(f"hit: {m[1]}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught: Map key not found\nhit: 10\n");
    }

    TEST_CASE_TEMPLATE("m[k] on an empty map throws KeyError", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            try {
                var v: i32 = m["absent"];
                print("unreachable");
            } catch (e: KeyError) {
                print("caught empty");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught empty\n");
    }

    TEST_CASE_TEMPLATE("string-valued map missing key throws KeyError", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, string> = Map<i32, string>();
            m.insert(1, "one");
            print(m[1]);
            try {
                print(m[2]);
            } catch (e: KeyError) {
                print("caught string-value miss");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "one\ncaught string-value miss\n");
    }

    TEST_CASE_TEMPLATE("struct-valued map: hit reads, miss throws KeyError", Backend,
                       RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Pt { pub x: i32; pub y: i32; }
        fun main(): i32 {
            var m: Map<i32, Pt> = Map<i32, Pt>();
            m.insert(1, Pt { x = 3, y = 4 });
            var hit: Pt = m[1];
            print(f"{hit.x},{hit.y}");
            try {
                var miss: Pt = m[2];
                print("unreachable");
            } catch (e: KeyError) {
                print("caught struct-value miss");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3,4\ncaught struct-value miss\n");
    }

    TEST_CASE_TEMPLATE("catch-all catches an index exception", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            try {
                var v: i32 = m[5];
            } catch (e) {
                print("caught via catch-all");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught via catch-all\n");
    }

    TEST_CASE_TEMPLATE("index exception propagates through a call", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun lookup(m: inout Map<i32, i32>, k: i32): i32 {
            return m[k];
        }
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 100);
            try {
                var v: i32 = lookup(inout m, 42);
                print("unreachable");
            } catch (e: KeyError) {
                print("caught across frames");
            }
            print(f"{lookup(inout m, 1)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "caught across frames\n100\n");
    }

    // VM-only: an uncaught exception surfaces as a graceful "Unhandled exception"
    // (return false / nonzero exit) rather than a hard abort. The C backend's
    // uncaught-exception exit path differs, so this asserts VM behavior only.
    TEST_CASE("uncaught index exception fails gracefully") { // VM-only
        const char* source = R"(
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            var v: i32 = xs[0];
            return v;
        }
    )";

        auto result = VMBackend::run(source);
        CHECK_FALSE(result.success);
    }

} // TEST_SUITE("E2E Index Exceptions")

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST_SUITE("E2E Exceptions") {

    TEST_CASE_TEMPLATE("Exception basic throw/catch", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun risky(): i32 {
            throw MyError { code = 42 };
            return 0;
        }

        fun main(): i32 {
            try {
                var x: i32 = risky();
                return x;
            } catch (e: MyError) {
                return e.code;
            }
            return -1;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Exception trait validation error", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Throw non-Exception type") {
            const char* source = R"(
            struct NotAnException {
                x: i32;
            }

            fun main(): i32 {
                throw NotAnException { x = 1 };
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(!result.success);
        }

        SUBCASE("Catch non-Exception type") {
            const char* source = R"(
            struct NotAnException {
                x: i32;
            }

            fun main(): i32 {
                try {
                    var x: i32 = 1;
                } catch (e: NotAnException) {
                    return -1;
                }
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(!result.success);
        }
    }

    TEST_CASE_TEMPLATE("Exception catch-all", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct SomeError {
            val: i32;
        }

        fun SomeError.message(): string for Exception {
            return "some error";
        }

        fun risky(): i32 {
            throw SomeError { val = 99 };
            return 0;
        }

        fun main(): i32 {
            try {
                risky();
            } catch (e) {
                return 1;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE_TEMPLATE("Exception multiple catch clauses", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct ErrorA {
            code: i32;
        }

        fun ErrorA.message(): string for Exception {
            return "error a";
        }

        struct ErrorB {
            code: i32;
        }

        fun ErrorB.message(): string for Exception {
            return "error b";
        }

        fun throw_b(): i32 {
            throw ErrorB { code = 20 };
            return 0;
        }

        fun main(): i32 {
            try {
                throw_b();
            } catch (e: ErrorA) {
                return e.code;
            } catch (e: ErrorB) {
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 20);
    }

    TEST_CASE_TEMPLATE("Exception catch-all must be last", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct MyError {
            x: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun main(): i32 {
            try {
                var x: i32 = 1;
            } catch (e) {
                return -1;
            } catch (e: MyError) {
                return -2;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE_TEMPLATE("Exception finally on normal exit", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct MyError {
            x: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun main(): i32 {
            var result: i32 = 0;
            try {
                result = 10;
            } catch (e: MyError) {
                result = -1;
            } finally {
                result = result + 1;
            }
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 11);
    }

    TEST_CASE_TEMPLATE("Exception finally on exception", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct MyError {
            x: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun thrower(): i32 {
            throw MyError { x = 5 };
            return 0;
        }

        fun main(): i32 {
            var result: i32 = 0;
            try {
                thrower();
                result = 10;
            } catch (e: MyError) {
                result = e.x;
            } finally {
                result = result + 100;
            }
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 105);
    }

    TEST_CASE_TEMPLATE("Exception stack unwinding through multiple frames", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct DeepError {
            level: i32;
        }

        fun DeepError.message(): string for Exception {
            return "deep error";
        }

        fun level3(): i32 {
            throw DeepError { level = 3 };
            return 0;
        }

        fun level2(): i32 {
            return level3();
        }

        fun level1(): i32 {
            return level2();
        }

        fun main(): i32 {
            try {
                return level1();
            } catch (e: DeepError) {
                return e.level;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("Exception nested try/catch", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct InnerError {
            x: i32;
        }

        fun InnerError.message(): string for Exception {
            return "inner";
        }

        struct OuterError {
            x: i32;
        }

        fun OuterError.message(): string for Exception {
            return "outer";
        }

        fun throw_inner(): i32 {
            throw InnerError { x = 10 };
            return 0;
        }

        fun main(): i32 {
            try {
                try {
                    throw_inner();
                } catch (e: InnerError) {
                    return e.x;
                }
            } catch (e: OuterError) {
                return -1;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
    }

    TEST_CASE("Unhandled exception") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* source = R"(
        struct FatalError {
            code: i32;
        }

        fun FatalError.message(): string for Exception {
            return "fatal";
        }

        fun main(): i32 {
            throw FatalError { code = 1 };
            return 0;
        }
    )";

        auto result = VMBackend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE_TEMPLATE("Exception with fields accessible in catch", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct ValueError {
            code: i32;
            extra: i32;
        }

        fun ValueError.message(): string for Exception {
            return "value error";
        }

        fun fail(): i32 {
            throw ValueError { code = 7, extra = 13 };
            return 0;
        }

        fun main(): i32 {
            try {
                fail();
            } catch (e: ValueError) {
                return e.code + e.extra;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 20);
    }

    TEST_CASE_TEMPLATE("Exception try without catch (finally only)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var result: i32 = 0;
            try {
                result = 42;
            } finally {
                result = result + 1;
            }
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 43);
    }

    TEST_CASE_TEMPLATE("Exception normal flow no exception thrown", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct MyError {
            x: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun safe(): i32 {
            return 42;
        }

        fun main(): i32 {
            try {
                var x: i32 = safe();
                return x;
            } catch (e: MyError) {
                return -1;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Exception typed catch mismatch falls through", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct ErrorA {
            x: i32;
        }

        fun ErrorA.message(): string for Exception {
            return "a";
        }

        struct ErrorB {
            x: i32;
        }

        fun ErrorB.message(): string for Exception {
            return "b";
        }

        fun throw_b(): i32 {
            throw ErrorB { x = 5 };
            return 0;
        }

        fun main(): i32 {
            try {
                throw_b();
            } catch (e: ErrorA) {
                return -1;
            } catch (e) {
                return 99;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    TEST_CASE_TEMPLATE("Exception throw in directly in try block", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct SimpleError {
            val: i32;
        }

        fun SimpleError.message(): string for Exception {
            return "simple";
        }

        fun main(): i32 {
            try {
                throw SimpleError { val = 77 };
            } catch (e: SimpleError) {
                return e.val;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 77);
    }

    // ============================================================================
    // Exception Safety Tests - RAII Cleanup During Exception Handling
    // ============================================================================

    TEST_CASE_TEMPLATE("Exception safety: throw cleans up current scope uniq", Backend, RX_E2E_BACKENDS) {
        // A uniq variable in the same scope as a throw should be cleaned up
        const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"{"~Resource"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun main(): i32 {
            try {
                var r: uniq Resource = uniq Resource();
                r.id = 1;
                throw MyError { code = 42 };
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
        // Resource destructor should run before catch handler
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: catch handler cleans up try scope", Backend, RX_E2E_BACKENDS) {
        // A uniq variable declared in try body should be cleaned up when a called
        // function throws an exception that is caught
        const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"{"~Resource"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun risky() {
            throw MyError { code = 99 };
        }

        fun main(): i32 {
            try {
                var r: uniq Resource = uniq Resource();
                r.id = 1;
                risky();
                return -1;
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: cross-frame unwinding cleanup", Backend, RX_E2E_BACKENDS) {
        // A uniq variable in an intermediate function (no handler) should be cleaned
        // up when the exception propagates through
        const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"{"~Resource"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun thrower() {
            throw MyError { code = 7 };
        }

        fun middle() {
            var r: uniq Resource = uniq Resource();
            r.id = 2;
            thrower();
        }

        fun main(): i32 {
            try {
                middle();
                return -1;
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: LIFO cleanup order", Backend, RX_E2E_BACKENDS) {
        // Multiple uniq variables should be cleaned up in reverse declaration order
        const char* source = R"(
        struct A {
            val: i32;
        }

        fun delete A() {
            print(f"{"~A"}");
        }

        struct B {
            val: i32;
        }

        fun delete B() {
            print(f"{"~B"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun risky() {
            throw MyError { code = 1 };
        }

        fun main(): i32 {
            try {
                var a: uniq A = uniq A();
                var b: uniq B = uniq B();
                risky();
                return -1;
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
        // b destroyed first (LIFO), then a
        CHECK(result.stdout_output == "~B\n~A\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: nested try/catch cleanup", Backend, RX_E2E_BACKENDS) {
        // Inner and outer scopes should both be cleaned up correctly
        const char* source = R"(
        struct Outer {
            val: i32;
        }

        fun delete Outer() {
            print(f"{"~Outer"}");
        }

        struct Inner {
            val: i32;
        }

        fun delete Inner() {
            print(f"{"~Inner"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun risky() {
            throw MyError { code = 5 };
        }

        fun work() {
            var outer: uniq Outer = uniq Outer();
            try {
                var inner: uniq Inner = uniq Inner();
                risky();
            } catch (e: MyError) {
                print(f"{"inner caught"}");
            }
        }

        fun main(): i32 {
            work();
            print(f"{"done"}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 0);
        // Inner is cleaned up by exception handler, Outer is cleaned up at scope exit
        CHECK(result.stdout_output == "~Inner\ninner caught\n~Outer\ndone\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: value struct destructor during unwinding", Backend, RX_E2E_BACKENDS) {
        // A value struct with a custom destructor should have its destructor
        // called during exception unwinding
        const char* source = R"(
        struct Guard {
            name: i32;
        }

        fun delete Guard() {
            print(f"{"~Guard"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun risky() {
            throw MyError { code = 3 };
        }

        fun guarded() {
            var g: Guard = Guard { name = 1 };
            risky();
        }

        fun main(): i32 {
            try {
                guarded();
                return -1;
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
        CHECK(result.stdout_output == "~Guard\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: finally on exception path", Backend, RX_E2E_BACKENDS) {
        // A finally block should execute when an exception propagates through
        const char* source = R"(
        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun main(): i32 {
            var result: i32 = 0;
            try {
                try {
                    throw MyError { code = 10 };
                } finally {
                    print(f"{"finally"}");
                }
            } catch (e: MyError) {
                result = e.code;
                print(f"{"caught"}");
            }
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
        // Finally should run before the exception is re-thrown to outer catch
        CHECK(result.stdout_output == "finally\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: already-moved uniq skipped during cleanup", Backend, RX_E2E_BACKENDS) {
        // A uniq variable that was moved before the throw should not be double-freed
        const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"{"~Resource"}");
        }

        struct MyError {
            code: i32;
        }

        fun MyError.message(): string for Exception {
            return "error";
        }

        fun consume(r: uniq Resource): i32 {
            return r.id;
        }

        fun main(): i32 {
            try {
                var r: uniq Resource = uniq Resource();
                r.id = 42;
                var val: i32 = consume(r);
                // r is moved - should not be cleaned up during exception
                throw MyError { code = val };
            } catch (e: MyError) {
                print(f"{"caught"}");
                return e.code;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
        // Resource is destroyed by consume(), NOT by exception cleanup
        // Only one destructor call expected
        CHECK(result.stdout_output == "~Resource\ncaught\n");
    }

    TEST_CASE_TEMPLATE("Exception safety: normal path still works", Backend, RX_E2E_BACKENDS) {
        // Verify no regressions for non-exception RAII cleanup
        const char* source = R"(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"{"~Resource"}");
        }

        fun work(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 10;
            return r.id;
        }

        fun main(): i32 {
            var result: i32 = work();
            print(f"{result}");
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
        CHECK(result.stdout_output == "~Resource\n10\n");
    }

    TEST_CASE_TEMPLATE("Exception move tracking: moved in try, used in catch", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print(f"{"~Resource"}"); }
        fun consume(r: uniq Resource): i32 { return r.id; }

        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 42;
            try {
                var val: i32 = consume(r);
                throw MyError { code = val };
            } catch (e: MyError) {
                return r.id;
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE_TEMPLATE("Exception move tracking: moved in try, reassigned in catch, used after", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print(f"{"~Resource"}"); }
        fun consume(r: uniq Resource): i32 { return r.id; }

        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 42;
            try {
                var val: i32 = consume(r);
                throw MyError { code = val };
            } catch (e: MyError) {
                r = uniq Resource();
                r.id = 99;
            }
            return r.id;
        }
    )";

        // Try body ends with an unconditional throw, so its normal-exit path
        // (r = Moved) is unreachable after the try/catch. Only the catch path
        // survives (r reassigned, Live), so this program is well-typed and
        // returns 99.
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    TEST_CASE_TEMPLATE("Exception move tracking: not moved in try, moved in catch, used after", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print(f"{"~Resource"}"); }
        fun consume(r: uniq Resource): i32 { return r.id; }

        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 42;
            try {
                throw MyError { code = 1 };
            } catch (e: MyError) {
                var val: i32 = consume(r);
            }
            return r.id;
        }
    )";

        // post-try: Live, post-catch: Moved → merged = MaybeValid
        auto result = Backend::run(source);
        CHECK(!result.success);
    }

    TEST_CASE_TEMPLATE("Exception move tracking: no move anywhere", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print(f"{"~Resource"}"); }

        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 42;
            try {
                throw MyError { code = 1 };
            } catch (e: MyError) {
                r.id = 99;
            }
            return r.id;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
    }

    TEST_CASE_TEMPLATE("Exception move tracking: no leak between catch clauses", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { print(f"{"~Resource"}"); }
        fun consume(r: uniq Resource): i32 { return r.id; }

        struct MyError { code: i32; }
        fun MyError.message(): string for Exception { return "error"; }

        struct OtherError { code: i32; }
        fun OtherError.message(): string for Exception { return "other"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 42;
            try {
                throw MyError { code = 1 };
            } catch (e: MyError) {
                var val: i32 = consume(r);
            } catch (e: OtherError) {
                return r.id;
            }
            return 0;
        }
    )";

        // Second catch starts from catch_entry (not first catch's exit).
        // r is not moved in try, so catch_entry has r as Live.
        // The second catch can access r.id without error — the first catch's
        // move does not leak into the second catch.
        // r is not accessed after the try/catch, so the merged MaybeValid state is fine.
        auto result = Backend::run(source);
        CHECK(result.success);
    }

    // ============================================================================
    // Container cleanup on exception unwind
    // ============================================================================

    TEST_CASE_TEMPLATE("Exception cleanup: List<uniq T> elements destroyed on unwind", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Widget {
            id: i32;
        }

        fun delete Widget() {
            print(f"del:{self.id}");
        }

        struct Boom {
            code: i32;
        }
        fun Boom.message(): string for Exception {
            return "boom";
        }

        fun explode() {
            throw Boom { code = 1 };
        }

        fun main(): i32 {
            try {
                var items: List<uniq Widget> = List<uniq Widget>();
                items.push(uniq Widget { id = 10 });
                items.push(uniq Widget { id = 20 });
                items.push(uniq Widget { id = 30 });
                explode();
                return -1;
            } catch (e: Boom) {
                return e.code;
            }
            return -2;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
        // Destructors should have been called for all 3 widgets during unwind
        CHECK(result.stdout_output.find("del:10") != String::npos);
        CHECK(result.stdout_output.find("del:20") != String::npos);
        CHECK(result.stdout_output.find("del:30") != String::npos);
    }

    TEST_CASE_TEMPLATE("Exception cleanup: Map<string, uniq T> values destroyed on unwind", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource() {
            print(f"free:{self.value}");
        }

        struct Fail {
            x: i32;
        }
        fun Fail.message(): string for Exception {
            return "fail";
        }

        fun main(): i32 {
            try {
                var m: Map<string, uniq Resource> = Map<string, uniq Resource>();
                m.insert("a", uniq Resource { value = 100 });
                m.insert("b", uniq Resource { value = 200 });
                throw Fail { x = 42 };
                return -1;
            } catch (e: Fail) {
                return e.x;
            }
            return -2;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
        CHECK(result.stdout_output.find("free:100") != String::npos);
        CHECK(result.stdout_output.find("free:200") != String::npos);
    }

    TEST_CASE_TEMPLATE("Exception cleanup: temporary uniq destroyed on unwind", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Widget {
            id: i32;
        }

        fun delete Widget() {
            print(f"del:{self.id}");
        }

        struct Boom {
            code: i32;
        }
        fun Boom.message(): string for Exception {
            return "boom";
        }

        fun consume(w: uniq Widget): i32 {
            throw Boom { code = 1 };
            return 0;
        }

        fun main(): i32 {
            try {
                consume(uniq Widget { id = 99 });
                return -1;
            } catch (e: Boom) {
                return e.code;
            }
            return -2;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
        // The temporary uniq Widget should be cleaned up during exception unwind
        CHECK(result.stdout_output.find("del:99") != String::npos);
    }

    // ============================================================================
    // Throw in delete destructor (compile-time ban)
    // ============================================================================

    TEST_CASE_TEMPLATE("Exception throw in delete destructor rejected", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Direct throw in delete destructor") {
            const char* source = R"(
            struct MyError {
                code: i32;
            }

            fun MyError.message(): string for Exception {
                return "error";
            }

            struct Widget {
                id: i32;
            }

            fun delete Widget() {
                throw MyError { code = 1 };
            }

            fun main(): i32 {
                var w: uniq Widget = uniq Widget { id = 1 };
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(!result.success);
        }

        SUBCASE("Throw in named destructor is allowed") {
            const char* source = R"(
            struct MyError {
                code: i32;
            }

            fun MyError.message(): string for Exception {
                return "error";
            }

            struct Widget {
                id: i32;
            }

            fun delete Widget.close() {
                throw MyError { code = 1 };
            }

            fun main(): i32 {
                var w: uniq Widget = uniq Widget { id = 1 };
                try {
                    delete w.close();
                } catch (e: MyError) {
                    return e.code;
                }
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.value == 1);
        }
    }

    // ============================================================================
    // Pre-declared local reassigned from a throwing call inside try/catch
    // ============================================================================
    // The IR builder must not let the try-body's rebinding of `r` to the call's
    // (never-produced) result leak into the catch handler — otherwise the catch
    // dereferences an uninitialized SSA value (segfault for struct returns,
    // silently-wrong value for primitives).

    TEST_CASE_TEMPLATE("Try/catch: throwing call to pre-declared struct local preserves prior value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Pt { x: i32; y: i32; z: i32; }
        struct E { c: i32; }
        fun E.message(): string for Exception { return "e"; }

        fun inner(): Pt {
            throw E { c = 1 };
            return Pt { x = 99, y = 99, z = 99 };
        }

        fun main(): i32 {
            var r: Pt = Pt { x = 7, y = 7, z = 7 };
            try {
                r = inner();
            } catch (e: E) {
            }
            return r.x;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("Try/catch: throwing call to pre-declared primitive local preserves prior value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct E { c: i32; }
        fun E.message(): string for Exception { return "e"; }

        fun inner(): i32 {
            throw E { c = 1 };
            return 99;
        }

        fun main(): i32 {
            var r: i32 = 7;
            try {
                r = inner();
            } catch (e: E) {
            }
            return r;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("Try/catch: non-throwing call still rebinds the pre-declared local", Backend, RX_E2E_BACKENDS) {
        // Sanity check that the IR-builder rollback doesn't break the happy path:
        // when the call succeeds, the assignment must take effect.
        const char* source = R"ROXY(
        struct Pt { x: i32; y: i32; z: i32; }
        struct E { c: i32; }
        fun E.message(): string for Exception { return "e"; }

        fun inner(): Pt {
            return Pt { x = 42, y = 42, z = 42 };
        }

        fun main(): i32 {
            var r: Pt = Pt { x = 7, y = 7, z = 7 };
            try {
                r = inner();
            } catch (e: E) {
            }
            return r.x;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Try bodies containing loops (regression: RPO reorder placed a loop body
    // block after the loop's fall-through in the bytecode layout, so the original
    // single-range handler table missed the call site inside the loop and the
    // exception surfaced as "Unhandled exception" instead of being caught.)
    // ============================================================================

    TEST_CASE_TEMPLATE("Try/catch around while loop catches throw from inside", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }

        fun deep() {
            throw Err { code = 1 };
        }

        fun main(): i32 {
            try {
                var i: i32 = 0;
                while (i < 3) {
                    deep();
                    i = i + 1;
                }
            } catch (e: Err) {
                return 42;
            }
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Try/catch around for loop catches throw from inside", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }

        fun deep() {
            throw Err { code = 1 };
        }

        fun main(): i32 {
            try {
                for (var i: i32 = 0; i < 3; i = i + 1) {
                    deep();
                }
            } catch (e: Err) {
                return 42;
            }
            return 0;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Try/catch around loop: normal exit still reaches code after", Backend, RX_E2E_BACKENDS) {
        // Make sure the per-range handler table doesn't accidentally catch past
        // the try when the loop runs to completion without throwing.
        const char* source = R"ROXY(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }

        fun main(): i32 {
            var sum: i32 = 0;
            try {
                for (var i: i32 = 1; i <= 3; i = i + 1) {
                    sum = sum + i;
                }
            } catch (e: Err) {
                return -1;
            }
            return sum;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE_TEMPLATE("Try/catch around nested loops catches throw from inner iteration", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Err { code: i32; }
        fun Err.message(): string for Exception { return "boom"; }

        fun deep(i: i32, j: i32) {
            if (i == 1 && j == 2) {
                throw Err { code = 99 };
            }
        }

        fun main(): i32 {
            try {
                for (var i: i32 = 0; i < 3; i = i + 1) {
                    for (var j: i32 = 0; j < 3; j = j + 1) {
                        deep(i, j);
                    }
                }
            } catch (e: Err) {
                return e.code;
            }
            return -1;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

}  // TEST_SUITE("E2E Exceptions")

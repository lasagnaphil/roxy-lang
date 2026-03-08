#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Exception Handling Tests
// ============================================================================

TEST_CASE("E2E - Exception basic throw/catch") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Exception trait validation error") {
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

        TestResult result = run_and_capture(source, "main");
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

        TestResult result = run_and_capture(source, "main");
        CHECK(!result.success);
    }
}

TEST_CASE("E2E - Exception catch-all") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Exception multiple catch clauses") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 20);
}

TEST_CASE("E2E - Exception catch-all must be last") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Exception finally on normal exit") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 11);
}

TEST_CASE("E2E - Exception finally on exception") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 105);
}

TEST_CASE("E2E - Exception stack unwinding through multiple frames") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
}

TEST_CASE("E2E - Exception nested try/catch") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
}

TEST_CASE("E2E - Unhandled exception") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Exception with fields accessible in catch") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 20);
}

TEST_CASE("E2E - Exception try without catch (finally only)") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 43);
}

TEST_CASE("E2E - Exception normal flow no exception thrown") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Exception typed catch mismatch falls through") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
}

TEST_CASE("E2E - Exception throw in directly in try block") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 77);
}

// ============================================================================
// Exception Safety Tests - RAII Cleanup During Exception Handling
// ============================================================================

TEST_CASE("E2E - Exception safety: throw cleans up current scope uniq") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
    // Resource destructor should run before catch handler
    CHECK(result.stdout_output == "~Resource\ncaught\n");
}

TEST_CASE("E2E - Exception safety: catch handler cleans up try scope") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 99);
    CHECK(result.stdout_output == "~Resource\ncaught\n");
}

TEST_CASE("E2E - Exception safety: cross-frame unwinding cleanup") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 7);
    CHECK(result.stdout_output == "~Resource\ncaught\n");
}

TEST_CASE("E2E - Exception safety: LIFO cleanup order") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
    // b destroyed first (LIFO), then a
    CHECK(result.stdout_output == "~B\n~A\ncaught\n");
}

TEST_CASE("E2E - Exception safety: nested try/catch cleanup") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 0);
    // Inner is cleaned up by exception handler, Outer is cleaned up at scope exit
    CHECK(result.stdout_output == "~Inner\ninner caught\n~Outer\ndone\n");
}

TEST_CASE("E2E - Exception safety: value struct destructor during unwinding") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 3);
    CHECK(result.stdout_output == "~Guard\ncaught\n");
}

TEST_CASE("E2E - Exception safety: finally on exception path") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
    // Finally should run before the exception is re-thrown to outer catch
    CHECK(result.stdout_output == "finally\ncaught\n");
}

TEST_CASE("E2E - Exception safety: already-moved uniq skipped during cleanup") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
    // Resource is destroyed by consume(), NOT by exception cleanup
    // Only one destructor call expected
    CHECK(result.stdout_output == "~Resource\ncaught\n");
}

TEST_CASE("E2E - Exception safety: normal path still works") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
    CHECK(result.stdout_output == "~Resource\n10\n");
}

TEST_CASE("E2E - Exception move tracking: moved in try, used in catch") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Exception move tracking: moved in try, reassigned in catch, used after") {
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

    // post-try: Moved, post-catch: Live → merged = MaybeValid
    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Exception move tracking: not moved in try, moved in catch, used after") {
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
    TestResult result = run_and_capture(source, "main");
    CHECK(!result.success);
}

TEST_CASE("E2E - Exception move tracking: no move anywhere") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
}

TEST_CASE("E2E - Exception move tracking: no leak between catch clauses") {
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
    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
}


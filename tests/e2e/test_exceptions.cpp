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

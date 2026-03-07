#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

TEST_CASE("E2E - C Backend: Return constant") {
    const char* source = R"(
        fun main(): i32 {
            return 42;
        }
    )";

    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Integer arithmetic") {
    SUBCASE("Addition") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 10;
                var b: i32 = 20;
                return a + b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 30);
    }

    SUBCASE("Subtraction") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 50;
                var b: i32 = 8;
                return a - b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Multiplication") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 6;
                var b: i32 = 7;
                return a * b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Division") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 84;
                var b: i32 = 2;
                return a / b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 42);
    }

    SUBCASE("Modulo") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 47;
                var b: i32 = 5;
                return a % b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 2);
    }
}

TEST_CASE("E2E - C Backend: Negation") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = -42;
            return -a;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Comparisons and boolean logic") {
    SUBCASE("Less than true") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 5;
                var b: i32 = 10;
                if (a < b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Greater than false") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 5;
                var b: i32 = 10;
                if (a > b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0);
    }

    SUBCASE("Equality") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 42;
                var b: i32 = 42;
                if (a == b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Boolean AND") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = true;
                var b: bool = false;
                if (a && b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0);
    }

    SUBCASE("Boolean OR") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = true;
                var b: bool = false;
                if (a || b) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }

    SUBCASE("Boolean NOT") {
        const char* source = R"(
            fun main(): i32 {
                var a: bool = false;
                if (!a) { return 1; }
                return 0;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 1);
    }
}

TEST_CASE("E2E - C Backend: If/else control flow") {
    const char* source = R"(
        fun main(): i32 {
            var x: i32 = 10;
            if (x > 5) {
                return 1;
            } else {
                return 0;
            }
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 1);
}

TEST_CASE("E2E - C Backend: While loop") {
    const char* source = R"(
        fun main(): i32 {
            var sum: i32 = 0;
            var i: i32 = 1;
            while (i <= 10) {
                sum = sum + i;
                i = i + 1;
            }
            return sum;
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 55);
}

TEST_CASE("E2E - C Backend: Simple function call") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
        fun main(): i32 {
            return add(20, 22);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: Nested function calls") {
    const char* source = R"(
        fun double_val(x: i32): i32 {
            return x * 2;
        }
        fun add_one(x: i32): i32 {
            return x + 1;
        }
        fun main(): i32 {
            return add_one(double_val(20));
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 41);
}

TEST_CASE("E2E - C Backend: Recursive function") {
    const char* source = R"(
        fun factorial(n: i32): i32 {
            if (n <= 1) { return 1; }
            return n * factorial(n - 1);
        }
        fun main(): i32 {
            return factorial(5);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 120);
}

TEST_CASE("E2E - C Backend: Bitwise operations") {
    SUBCASE("AND") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xFF;
                var b: i32 = 0x0F;
                return a & b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0x0F);
    }

    SUBCASE("OR") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xF0;
                var b: i32 = 0x0F;
                return a | b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0xFF);
    }

    SUBCASE("XOR") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 0xFF;
                var b: i32 = 0xF0;
                return a ^ b;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 0x0F);
    }

    SUBCASE("Shift left") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 1;
                return a << 4;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 16);
    }

    SUBCASE("Shift right") {
        const char* source = R"(
            fun main(): i32 {
                var a: i32 = 64;
                return a >> 2;
            }
        )";
        CBackendResult result = compile_and_run_cpp(source);
        CHECK(result.compile_success);
        CHECK(result.run_success);
        CHECK(result.exit_code == 16);
    }
}

TEST_CASE("E2E - C Backend: Multiple functions") {
    const char* source = R"(
        fun min_val(a: i32, b: i32): i32 {
            if (a < b) { return a; }
            return b;
        }
        fun max_val(a: i32, b: i32): i32 {
            if (a > b) { return a; }
            return b;
        }
        fun clamp(x: i32, lo: i32, hi: i32): i32 {
            return min_val(max_val(x, lo), hi);
        }
        fun main(): i32 {
            return clamp(100, 0, 42);
        }
    )";
    CBackendResult result = compile_and_run_cpp(source);
    CHECK(result.compile_success);
    CHECK(result.run_success);
    CHECK(result.exit_code == 42);
}

TEST_CASE("E2E - C Backend: compile_to_cpp produces valid output") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }
        fun main(): i32 {
            return add(1, 2);
        }
    )";

    String cpp_source = compile_to_cpp(source);
    CHECK(!cpp_source.empty());
    CHECK(cpp_source.find("#include <stdint.h>") != String::npos);
    CHECK(cpp_source.find("int32_t") != String::npos);
    CHECK(cpp_source.find("return") != String::npos);
}

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

#include <string>

using namespace rx;

// ============================================================================
// Basic Tests
// ============================================================================

TEST_CASE("E2E - Return constant") {
    const char* source = R"(
        fun main(): i32 {
            return 42;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Arithmetic expressions") {
    SUBCASE("Addition") {
        const char* source = R"(
            fun main(): i32 {
                print(f"{10 + 32}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    SUBCASE("Complex expression") {
        const char* source = R"(
            fun main(): i32 {
                print(f"{1 + 2 * 3 + 4 * 5}");
                return 0;
            }
        )";
        // 1 + 6 + 20 = 27
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "27\n");
    }

    SUBCASE("Parentheses") {
        const char* source = R"(
            fun main(): i32 {
                print(f"{(1 + 2) * (3 + 4)}");
                return 0;
            }
        )";
        // 3 * 7 = 21
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "21\n");
    }

    SUBCASE("Division and modulo") {
        const char* source = R"(
            fun main(): i32 {
                print(f"{100 / 3}");
                print(f"{100 % 3}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "33\n1\n");
    }

    SUBCASE("Negation") {
        const char* source = R"(
            fun main(): i32 {
                print(f"{-42}");
                print(f"{-(-42)}");
                return 0;
            }
        )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success);
        CHECK(result.stdout_output == "-42\n42\n");
    }
}

TEST_CASE("E2E - Local variables") {
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 10;
            var b: i32 = 20;
            var c: i32 = a + b;
            print(f"{a}");
            print(f"{b}");
            print(f"{c}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n30\n");
}

TEST_CASE("E2E - Function calls") {
    const char* source = R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }

        fun main(): i32 {
            print(f"{add(17, 25)}");
            print(f"{add(100, 200)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n300\n");
}

// ============================================================================
// Control Flow Tests
// ============================================================================

TEST_CASE("E2E - If statement") {
    const char* source = R"(
        fun abs(x: i32): i32 {
            if (x < 0) {
                return -x;
            }
            return x;
        }

        fun main(): i32 {
            print(f"{abs(42)}");
            print(f"{abs(-42)}");
            print(f"{abs(0)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n42\n0\n");
}

TEST_CASE("E2E - If-else statement") {
    const char* source = R"(
        fun max(a: i32, b: i32): i32 {
            if (a > b) {
                return a;
            } else {
                return b;
            }
        }

        fun main(): i32 {
            print(f"{max(10, 5)}");
            print(f"{max(3, 7)}");
            print(f"{max(5, 5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n7\n5\n");
}

TEST_CASE("E2E - While loop") {
    const char* source = R"(
        fun sum_to_n(n: i32): i32 {
            var sum: i32 = 0;
            var i: i32 = 1;
            while (i <= n) {
                sum = sum + i;
                i = i + 1;
            }
            return sum;
        }

        fun main(): i32 {
            print(f"{sum_to_n(10)}");
            print(f"{sum_to_n(100)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "55\n5050\n");
}

TEST_CASE("E2E - For loop") {
    const char* source = R"(
        fun main(): i32 {
            var sum: i32 = 0;
            for (var i: i32 = 1; i <= 10; i = i + 1) {
                sum = sum + i;
                print(f"{i}");
            }
            print(f"{sum}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n55\n");
}

TEST_CASE("E2E - Nested loops") {
    const char* source = R"(
        fun main(): i32 {
            for (var i: i32 = 1; i <= 3; i = i + 1) {
                for (var j: i32 = 1; j <= 3; j = j + 1) {
                    print(f"{i * 10 + j}");
                }
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "11\n12\n13\n21\n22\n23\n31\n32\n33\n");
}

TEST_CASE("E2E - Boolean expressions") {
    // Test AND operator with comparison expressions
    // NOTE: OR operator has a known bug with short-circuit evaluation
    const char* source = R"(
        fun main(): i32 {
            var a: i32 = 5;
            var b: i32 = 3;

            // AND with comparisons - all work correctly
            if ((a > b) && (b > 0)) { print(f"{1}"); } else { print(f"{0}"); }
            if ((a > b) && (b < 0)) { print(f"{1}"); } else { print(f"{0}"); }
            if ((a < b) && (b > 0)) { print(f"{1}"); } else { print(f"{0}"); }
            if ((a < b) && (b < 0)) { print(f"{1}"); } else { print(f"{0}"); }

            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // AND: (5>3)&&(3>0)=1, (5>3)&&(3<0)=0, (5<3)&&(3>0)=0, (5<3)&&(3<0)=0
    CHECK(result.stdout_output == "1\n0\n0\n0\n");
}

TEST_CASE("E2E - Comparison operators") {
    const char* source = R"(
        fun bool_to_int(b: bool): i32 {
            if (b) { return 1; }
            return 0;
        }

        fun main(): i32 {
            print(f"{bool_to_int(5 == 5)}");
            print(f"{bool_to_int(5 == 3)}");
            print(f"{bool_to_int(5 != 3)}");
            print(f"{bool_to_int(5 != 5)}");
            print(f"{bool_to_int(3 < 5)}");
            print(f"{bool_to_int(5 < 3)}");
            print(f"{bool_to_int(5 <= 5)}");
            print(f"{bool_to_int(3 > 5)}");
            print(f"{bool_to_int(5 > 3)}");
            print(f"{bool_to_int(5 >= 5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // ==: 1,0; !=: 1,0; <: 1,0; <=: 1; >: 0,1; >=: 1
    CHECK(result.stdout_output == "1\n0\n1\n0\n1\n0\n1\n0\n1\n1\n");
}

// ============================================================================
// Register Limit Tests
// ============================================================================

// Helper: generate a Roxy function with N local i32 variables, sum them, and return the sum.
// Each variable v_i is assigned the value (i + 1).
// The function returns v_0 + v_1 + ... + v_{N-1} = N*(N+1)/2.
static std::string generate_many_locals(int count) {
    std::string src = "fun main(): i32 {\n";
    for (int i = 0; i < count; i++) {
        src += "    var v_" + std::to_string(i) + ": i32 = " + std::to_string(i + 1) + ";\n";
    }
    // Sum all variables into an accumulator
    src += "    var sum: i32 = 0;\n";
    for (int i = 0; i < count; i++) {
        src += "    sum = sum + v_" + std::to_string(i) + ";\n";
    }
    src += "    return sum;\n";
    src += "}\n";
    return src;
}

// Helper: generate a function with N locals that just returns the last one (no summing).
// This uses approximately N+1 registers (N locals + return value).
static std::string generate_locals_return_last(int count) {
    std::string src = "fun main(): i32 {\n";
    for (int i = 0; i < count; i++) {
        src += "    var v_" + std::to_string(i) + ": i32 = " + std::to_string(i + 1) + ";\n";
    }
    src += "    return v_" + std::to_string(count - 1) + ";\n";
    src += "}\n";
    return src;
}

TEST_CASE("E2E - Many local variables (near register limit)") {
    // The VM uses 8-bit register indices (0-254, with 0xFF as sentinel).
    // With liveness-based register allocation, temporaries from expressions
    // are reused once they're dead, so the effective limit is "peak
    // simultaneously-live values" rather than "total SSA values."

    SUBCASE("100 locals with summation") {
        std::string src = generate_many_locals(100);
        TestResult result = run_and_capture(src.c_str(), "main");
        CHECK(result.success);
        CHECK(result.value == 5050);  // 100*101/2
    }

    SUBCASE("200 locals with summation") {
        // With register reuse, add temporaries are recycled. Fits easily.
        std::string src = generate_many_locals(200);
        TestResult result = run_and_capture(src.c_str(), "main");
        CHECK(result.success);
        CHECK(result.value == 20100);  // 200*201/2
    }

    SUBCASE("253 locals with summation - at boundary") {
        // 253 locals + sum + 1 reused add temp = 255 peak. Just fits.
        std::string src = generate_many_locals(253);
        TestResult result = run_and_capture(src.c_str(), "main");
        CHECK(result.success);
        CHECK(result.value == 32131);  // 253*254/2
    }

    SUBCASE("254 locals with summation - overflow") {
        // 254 locals + sum + add temp = 256 peak. Exceeds 255 register limit.
        std::string src = generate_many_locals(254);
        TestResult result = run_and_capture(src.c_str(), "main");
        CHECK_FALSE(result.success);
    }

    SUBCASE("500 locals return-last - register reuse") {
        // Only the last local is used at the return; dead locals' registers
        // are recycled. Succeeds despite >255 total SSA values.
        std::string src = generate_locals_return_last(500);
        TestResult result = run_and_capture(src.c_str(), "main");
        CHECK(result.success);
        CHECK(result.value == 500);
    }
}

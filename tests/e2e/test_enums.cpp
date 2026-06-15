#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

#include <roxy/core/doctest/doctest.h>

using namespace rx;

TEST_SUITE("E2E Enums") {

    TEST_CASE_TEMPLATE("Enum basic definition", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Color { Red, Green, Blue }

        fun main(): i32 {
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 0);
    }

    TEST_CASE_TEMPLATE("Enum variant access", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Color { Red, Green, Blue }

        fun main(): i32 {
            var c: Color = Color::Red;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 0);
    }

    TEST_CASE_TEMPLATE("Enum comparison", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Different variants compare not equal") {
            const char* source = R"(
            enum Color { Red, Green, Blue }

            fun main(): i32 {
                var c: Color = Color::Green;
                if (c == Color::Red) {
                    print(f"{1}");
                } else {
                    print(f"{0}");
                }
                return 0;
            }
        )";

            // Green != Red, so should print 0
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "0\n");
        }

        SUBCASE("Same variants compare equal") {
            const char* source = R"(
            enum Color { Red, Green, Blue }

            fun main(): i32 {
                var c: Color = Color::Green;
                if (c == Color::Green) {
                    print(f"{1}");
                } else {
                    print(f"{0}");
                }
                return 0;
            }
        )";

            // Green == Green, so should print 1
            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "1\n");
        }
    }

    TEST_CASE_TEMPLATE("Enum explicit values", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Explicit value assignment") {
            const char* source = R"(
            enum Status { Pending = 0, Active = 10, Done = 20 }

            fun main(): i32 {
                var s: Status = Status::Active;
                if (s == Status::Active) {
                    print(f"{1}");
                } else {
                    print(f"{0}");
                }
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "1\n");
        }

        SUBCASE("Auto-increment after explicit value") {
            const char* source = R"(
            enum Code { A = 5, B, C }  // B=6, C=7

            fun main(): i32 {
                if (Code::A == Code::A) { print(f"{1}"); } else { print(f"{0}"); }
                if (Code::B == Code::B) { print(f"{1}"); } else { print(f"{0}"); }
                if (Code::C == Code::C) { print(f"{1}"); } else { print(f"{0}"); }
                return 0;
            }
        )";

            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.stdout_output == "1\n1\n1\n");
        }
    }

    TEST_CASE_TEMPLATE("Enum in struct field", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Direction { North, East, South, West }

        struct Player {
            x: i32;
            y: i32;
            facing: Direction;
        }

        fun main(): i32 {
            var p: Player = Player { x = 10, y = 20, facing = Direction::East };
            if (p.facing == Direction::East) {
                print(f"{1}");
            } else {
                print(f"{0}");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n");
    }

    TEST_CASE_TEMPLATE("Enum as function parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Op { Add, Sub, Mul }

        fun apply(a: i32, b: i32, op: Op): i32 {
            if (op == Op::Add) {
                return a + b;
            }
            if (op == Op::Sub) {
                return a - b;
            }
            return a * b;
        }

        fun main(): i32 {
            print(f"{apply(10, 3, Op::Add)}");
            print(f"{apply(10, 3, Op::Sub)}");
            print(f"{apply(10, 3, Op::Mul)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "13\n7\n30\n");
    }

}  // TEST_SUITE("E2E Enums")

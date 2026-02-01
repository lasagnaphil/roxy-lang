#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// When Statement Tests
// ============================================================================

TEST_CASE("E2E - When basic") {
    const char* source = R"(
        enum Color { Red, Green, Blue }

        fun test_color(c: Color): i32 {
            when c {
                case Red:
                    return 1;
                case Green:
                    return 2;
                case Blue:
                    return 3;
            }
            return 0;
        }

        fun main(): i32 {
            print(test_color(Color::Red));
            print(test_color(Color::Green));
            print(test_color(Color::Blue));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

TEST_CASE("E2E - When with else") {
    const char* source = R"(
        enum Status { Pending, Active, Done, Cancelled }

        fun status_code(s: Status): i32 {
            when s {
                case Pending:
                    return 100;
                case Active:
                    return 200;
                else:
                    return 999;
            }
        }

        fun main(): i32 {
            print(status_code(Status::Pending));
            print(status_code(Status::Active));
            print(status_code(Status::Done));
            print(status_code(Status::Cancelled));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n200\n999\n999\n");
}

TEST_CASE("E2E - When with multiple case names") {
    const char* source = R"(
        enum Day { Mon, Tue, Wed, Thu, Fri, Sat, Sun }

        fun is_weekend(d: Day): i32 {
            when d {
                case Sat, Sun:
                    return 1;
                else:
                    return 0;
            }
        }

        fun main(): i32 {
            print(is_weekend(Day::Mon));
            print(is_weekend(Day::Fri));
            print(is_weekend(Day::Sat));
            print(is_weekend(Day::Sun));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n0\n1\n1\n");
}

TEST_CASE("E2E - When with enum explicit values") {
    const char* source = R"(
        enum Priority { Low = 10, Medium = 50, High = 100 }

        fun get_multiplier(p: Priority): i32 {
            when p {
                case Low:
                    return 1;
                case Medium:
                    return 2;
                case High:
                    return 3;
            }
            return 0;
        }

        fun main(): i32 {
            print(get_multiplier(Priority::Low));
            print(get_multiplier(Priority::Medium));
            print(get_multiplier(Priority::High));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

TEST_CASE("E2E - When with local variables in case") {
    const char* source = R"(
        enum Op { Add, Sub, Mul }

        fun calc(op: Op, a: i32, b: i32): i32 {
            when op {
                case Add:
                    var result: i32 = a + b;
                    return result;
                case Sub:
                    var result: i32 = a - b;
                    return result;
                case Mul:
                    var result: i32 = a * b;
                    return result;
            }
            return 0;
        }

        fun main(): i32 {
            print(calc(Op::Add, 10, 3));
            print(calc(Op::Sub, 10, 3));
            print(calc(Op::Mul, 10, 3));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "13\n7\n30\n");
}

TEST_CASE("E2E - When with multiple statements per case") {
    // Note: This test uses return in each case because when doesn't yet
    // support phi nodes for variable modifications across cases
    const char* source = R"(
        enum Level { Debug, Info, Error }

        fun log_level(l: Level): i32 {
            when l {
                case Debug:
                    print(1);
                    return 10;
                case Info:
                    print(2);
                    return 20;
                case Error:
                    print(3);
                    return 30;
            }
            return 0;
        }

        fun main(): i32 {
            var c1: i32 = log_level(Level::Debug);
            var c2: i32 = log_level(Level::Info);
            var c3: i32 = log_level(Level::Error);
            print(c1);
            print(c2);
            print(c3);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n10\n20\n30\n");
}

TEST_CASE("E2E - When exhaustive without else") {
    const char* source = R"(
        enum Bool2 { True2, False2 }

        fun to_int(b: Bool2): i32 {
            when b {
                case True2:
                    return 1;
                case False2:
                    return 0;
            }
            return -1;
        }

        fun main(): i32 {
            print(to_int(Bool2::True2));
            print(to_int(Bool2::False2));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n0\n");
}

TEST_CASE("E2E - When with variable discriminant") {
    const char* source = R"(
        enum Mode { Read, Write, ReadWrite }

        fun check_mode(m: Mode): i32 {
            var mode: Mode = m;
            when mode {
                case Read:
                    return 1;
                case Write:
                    return 2;
                case ReadWrite:
                    return 3;
            }
            return 0;
        }

        fun main(): i32 {
            print(check_mode(Mode::Read));
            print(check_mode(Mode::Write));
            print(check_mode(Mode::ReadWrite));
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

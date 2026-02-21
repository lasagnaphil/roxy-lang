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
            print(f"{test_color(Color::Red)}");
            print(f"{test_color(Color::Green)}");
            print(f"{test_color(Color::Blue)}");
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
            print(f"{status_code(Status::Pending)}");
            print(f"{status_code(Status::Active)}");
            print(f"{status_code(Status::Done)}");
            print(f"{status_code(Status::Cancelled)}");
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
            print(f"{is_weekend(Day::Mon)}");
            print(f"{is_weekend(Day::Fri)}");
            print(f"{is_weekend(Day::Sat)}");
            print(f"{is_weekend(Day::Sun)}");
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
            print(f"{get_multiplier(Priority::Low)}");
            print(f"{get_multiplier(Priority::Medium)}");
            print(f"{get_multiplier(Priority::High)}");
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
            print(f"{calc(Op::Add, 10, 3)}");
            print(f"{calc(Op::Sub, 10, 3)}");
            print(f"{calc(Op::Mul, 10, 3)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "13\n7\n30\n");
}

TEST_CASE("E2E - When with multiple statements per case") {
    // This test verifies that variable modifications inside case bodies
    // persist after the when statement (phi node support)
    const char* source = R"(
        enum Level { Debug, Info, Error }

        fun log_level(l: Level): i32 {
            var code: i32 = 0;
            when l {
                case Debug:
                    print(f"{1}");
                    code = 10;
                case Info:
                    print(f"{2}");
                    code = 20;
                case Error:
                    print(f"{3}");
                    code = 30;
            }
            return code;
        }

        fun main(): i32 {
            var c1: i32 = log_level(Level::Debug);
            var c2: i32 = log_level(Level::Info);
            var c3: i32 = log_level(Level::Error);
            print(f"{c1}");
            print(f"{c2}");
            print(f"{c3}");
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
            print(f"{to_int(Bool2::True2)}");
            print(f"{to_int(Bool2::False2)}");
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
            print(f"{check_mode(Mode::Read)}");
            print(f"{check_mode(Mode::Write)}");
            print(f"{check_mode(Mode::ReadWrite)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n3\n");
}

TEST_CASE("E2E - When phi without else") {
    const char* source = R"(
        enum Op { Add, Sub }

        fun calc(op: Op, a: i32, b: i32): i32 {
            var result: i32 = a;
            when op {
                case Add:
                    result = a + b;
                case Sub:
                    result = a - b;
            }
            return result;
        }

        fun main(): i32 {
            print(f"{calc(Op::Add, 10, 5)}");
            print(f"{calc(Op::Sub, 10, 5)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "15\n5\n");
}

TEST_CASE("E2E - When phi with else") {
    const char* source = R"(
        enum Status { Ok, Warning, Error, Unknown }

        fun status_value(s: Status): i32 {
            var value: i32 = -1;
            when s {
                case Ok:
                    value = 0;
                case Warning:
                    value = 1;
                case Error:
                    value = 2;
                else:
                    value = 99;
            }
            return value;
        }

        fun main(): i32 {
            print(f"{status_value(Status::Ok)}");
            print(f"{status_value(Status::Warning)}");
            print(f"{status_value(Status::Error)}");
            print(f"{status_value(Status::Unknown)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "0\n1\n2\n99\n");
}

TEST_CASE("E2E - When phi multiple variables") {
    const char* source = R"(
        enum Action { Move, Jump, Attack }

        fun process_action(a: Action): i32 {
            var x: i32 = 0;
            var y: i32 = 0;
            when a {
                case Move:
                    x = 10;
                    y = 5;
                case Jump:
                    x = 0;
                    y = 20;
                case Attack:
                    x = 5;
                    y = 0;
            }
            return x + y;
        }

        fun main(): i32 {
            print(f"{process_action(Action::Move)}");
            print(f"{process_action(Action::Jump)}");
            print(f"{process_action(Action::Attack)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "15\n20\n5\n");
}

TEST_CASE("E2E - When phi partial coverage") {
    // Some cases modify the variable, some don't - should use original value
    const char* source = R"(
        enum Priority { Low, Medium, High }

        fun get_score(p: Priority, base: i32): i32 {
            var score: i32 = base;
            when p {
                case High:
                    score = base * 3;
                case Medium:
                    score = base * 2;
            }
            return score;
        }

        fun main(): i32 {
            print(f"{get_score(Priority::Low, 10)}");
            print(f"{get_score(Priority::Medium, 10)}");
            print(f"{get_score(Priority::High, 10)}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Low falls through to merge with original value (10)
    CHECK(result.stdout_output == "10\n20\n30\n");
}

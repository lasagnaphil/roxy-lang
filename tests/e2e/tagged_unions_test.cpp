#include "test_helpers.hpp"

#include <roxy/core/doctest/doctest.h>

using namespace rx;

TEST_CASE("E2E - Tagged unions basic definition") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: f32;
            }
        }

        fun main(): i32 {
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 0);
}

TEST_CASE("E2E - Tagged unions struct literal with variant A") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: f32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 42 };
            print(d.val_a);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Tagged unions struct literal with variant B") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::B, val_b = 99 };
            print(d.val_b);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "99\n");
}

TEST_CASE("E2E - Tagged unions with regular fields") {
    const char* source = R"(
        enum SkillType { Attack, Defend }

        struct Skill {
            name_id: i32;
            when type: SkillType {
                case Attack:
                    damage: i32;
                case Defend:
                    damage_reduce: i32;
            }
        }

        fun main(): i32 {
            var skill: Skill = Skill {
                name_id = 1,
                type = SkillType::Attack,
                damage = 100
            };
            print(skill.name_id);
            print(skill.damage);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n100\n");
}

TEST_CASE("E2E - Tagged unions multiple fields per variant") {
    const char* source = R"(
        enum Kind { Point2D, Point3D }

        struct Point {
            when kind: Kind {
                case Point2D:
                    x: i32;
                    y: i32;
                case Point3D:
                    px: i32;
                    py: i32;
                    pz: i32;
            }
        }

        fun main(): i32 {
            var p: Point = Point { kind = Kind::Point2D, x = 10, y = 20 };
            print(p.x);
            print(p.y);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Tagged unions variant field assignment") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 10 };
            d.val_a = 20;
            print(d.val_a);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n");
}

TEST_CASE("E2E - Tagged unions method access") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun Data.get_val_a(): i32 {
            return self.val_a;
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 42 };
            print(d.get_val_a());
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Tagged unions use with when statement") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d1: Data = Data { kind = Kind::A, val_a = 10 };
            when d1.kind {
                case A:
                    print(d1.val_a);
                case B:
                    print(d1.val_b);
            }

            var d2: Data = Data { kind = Kind::B, val_b = 20 };
            when d2.kind {
                case A:
                    print(d2.val_a);
                case B:
                    print(d2.val_b);
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

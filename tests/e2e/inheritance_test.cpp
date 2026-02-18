#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Inheritance Tests
// ============================================================================

TEST_CASE("E2E - Inherit field access") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            print(f"{d.hp}");
            print(f"{d.breed}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n5\n");
}

TEST_CASE("E2E - Inherit method from parent") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun Animal.get_hp(): i32 {
            return self.hp;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            print(f"{d.get_hp()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n");
}

TEST_CASE("E2E - Method override in child") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun Animal.speak(): i32 {
            return 1;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun Dog.speak(): i32 {
            return 2;
        }

        fun main(): i32 {
            var a: Animal = Animal { hp = 50 };
            var d: Dog = Dog { hp = 100, breed = 5 };
            print(f"{a.speak()}");
            print(f"{d.speak()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n");
}

TEST_CASE("E2E - Super method call") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun Animal.speak(): i32 {
            return 1;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun Dog.speak(): i32 {
            return super.speak() + 10;
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            print(f"{d.speak()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "11\n");
}

TEST_CASE("E2E - Constructor chaining implicit") {
    // Test implicit super() call to parent's default constructor
    // Note: implicit super() only works when parent has a default (parameterless) constructor
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun new Animal() {
            self.hp = 50;
            print(f"{1}");
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun new Dog(breed: i32) {
            // No explicit super() - will implicitly call Animal() default constructor
            self.breed = breed;
            print(f"{2}");
        }

        fun main(): i32 {
            var d: Dog = Dog(5);
            print(f"{d.hp}");
            print(f"{d.breed}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Parent default constructor called first (implicit super()), then child body
    CHECK(result.stdout_output == "1\n2\n50\n5\n");
}

TEST_CASE("E2E - Constructor chaining explicit super") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun new Animal(hp: i32) {
            self.hp = hp;
            print(f"{1}");
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun new Dog(hp: i32, breed: i32) {
            super(hp);
            self.breed = breed;
            print(f"{2}");
        }

        fun main(): i32 {
            var d: Dog = Dog(100, 5);
            print(f"{d.hp}");
            print(f"{d.breed}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n2\n100\n5\n");
}

TEST_CASE("E2E - Destructor chaining") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun delete Animal() {
            print(f"{1}");
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun delete Dog() {
            print(f"{2}");
        }

        fun main(): i32 {
            var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
            delete d;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Child destructor runs first, then parent destructor
    CHECK(result.stdout_output == "2\n1\n");
}

TEST_CASE("E2E - Value slicing on assignment") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun print_hp(a: Animal) {
            print(f"{a.hp}");
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            var a: Animal = d;
            print(f"{a.hp}");
            print_hp(d);
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n100\n");
}

TEST_CASE("E2E - Reference subtyping uniq to ref") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun print_animal(a: ref Animal) {
            print(f"{a.hp}");
        }

        fun main(): i32 {
            var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
            print_animal(d);
            delete d;
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n");
}

TEST_CASE("E2E - Multi-level inheritance") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun Animal.get_type(): i32 {
            return 1;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun Dog.get_type(): i32 {
            return 2;
        }

        struct Labrador : Dog {
            color: i32;
        }

        fun Labrador.get_type(): i32 {
            return super.get_type() + 10;
        }

        fun main(): i32 {
            var lab: Labrador = Labrador { hp = 100, breed = 5, color = 3 };
            print(f"{lab.hp}");
            print(f"{lab.breed}");
            print(f"{lab.color}");
            print(f"{lab.get_type()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "100\n5\n3\n12\n");
}

TEST_CASE("E2E - Synthesized constructor with inheritance") {
    const char* source = R"(
        struct Animal {
            hp: i32 = 50;
        }

        struct Dog : Animal {
            breed: i32 = 1;
        }

        fun main(): i32 {
            var d: Dog = Dog {};
            print(f"{d.hp}");
            print(f"{d.breed}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "50\n1\n");
}

TEST_CASE("E2E - Child accessing parent field in method") {
    const char* source = R"(
        struct Animal {
            hp: i32;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun Dog.get_stats(): i32 {
            return self.hp + self.breed;
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            print(f"{d.get_stats()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "105\n");
}

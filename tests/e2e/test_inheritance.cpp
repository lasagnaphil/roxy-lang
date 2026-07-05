#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Inheritance Tests
// ============================================================================

TEST_SUITE("E2E Inheritance") {

    TEST_CASE_TEMPLATE("Inherit field access", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n5\n");
    }

    TEST_CASE_TEMPLATE("Inherit method from parent", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }

    TEST_CASE_TEMPLATE("Method override in child", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n");
    }

    TEST_CASE_TEMPLATE("Super method call", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "11\n");
    }

    TEST_CASE_TEMPLATE("Super method call returning void", Backend, RX_E2E_BACKENDS) {
        // Regression: gen_super_call used to classify constructor-vs-method by
        // "the call's result type is void" — a void-returning super *method*
        // was therefore mangled as a constructor and the call failed to
        // resolve. The semantic analyzer now annotates the distinction
        // explicitly (CallExpr::constructor_name).
        const char* source = R"(
        struct Animal {
            hp: i32;
        }

        fun Animal.heal() {
            self.hp = self.hp + 10;
        }

        struct Dog : Animal {
            breed: i32;
        }

        fun Dog.heal() {
            super.heal();
            self.hp = self.hp + 1;
        }

        fun main(): i32 {
            var d: Dog = Dog { hp = 100, breed = 5 };
            d.heal();
            print(f"{d.hp}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "111\n");
    }

    TEST_CASE_TEMPLATE("Constructor chaining implicit", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // Parent default constructor called first (implicit super()), then child body
        CHECK(result.stdout_output == "1\n2\n50\n5\n");
    }

    TEST_CASE_TEMPLATE("Constructor chaining explicit super", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n100\n5\n");
    }

    TEST_CASE_TEMPLATE("Destructor chaining", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // Child destructor runs first, then parent destructor
        CHECK(result.stdout_output == "2\n1\n");
    }

    TEST_CASE_TEMPLATE("Value slicing on assignment", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n100\n");
    }

    TEST_CASE_TEMPLATE("Reference subtyping uniq to ref", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n");
    }

    TEST_CASE_TEMPLATE("Multi-level inheritance", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n5\n3\n12\n");
    }

    TEST_CASE_TEMPLATE("Synthesized constructor with inheritance", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n1\n");
    }

    TEST_CASE_TEMPLATE("Child accessing parent field in method", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "105\n");
    }

}  // TEST_SUITE("E2E Inheritance")

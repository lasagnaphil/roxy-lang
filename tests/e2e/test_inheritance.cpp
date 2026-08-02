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

    TEST_CASE_TEMPLATE("Destructor chaining when the parent has no destructor", Backend, RX_E2E_BACKENDS) {
        // Only structs that need one get a default destructor, so a plain value
        // struct parent has no function to chain to. Emitting the call anyway
        // failed the whole compile with "function not found during bytecode
        // lowering" — the case the test above never covered, since it gives
        // both levels a destructor.
        const char* source = R"(
        struct Entity { hp: i32; }
        struct Player : Entity { mana: i32; }

        fun new Player() { self.hp = 1; self.mana = 50; }
        fun delete Player() { print("player removed"); }

        fun main(): i32 {
            var p: uniq Player = uniq Player();
            delete p;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "player removed\n");
    }

    TEST_CASE_TEMPLATE("Inherited destructor runs when the child declares none", Backend, RX_E2E_BACKENDS) {
        // A struct with nothing of its own to drop still needs a destructor to
        // carry the chain upward. Without one it had no destructor at all and
        // the ancestor's never ran — RAII silently skipped, no diagnostic.
        const char* source = R"(
        struct A { a: i32; }
        fun delete A() { print("A gone"); }

        struct B : A { b: i32; }          // no destructor, nothing to drop
        struct C : B { c: i32; }          // likewise

        fun new C() { self.a = 1; self.b = 2; self.c = 3; }

        fun main(): i32 {
            var x: uniq C = uniq C();
            delete x;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "A gone\n");
    }

    TEST_CASE_TEMPLATE("Inherited owned field is destroyed exactly once", Backend, RX_E2E_BACKENDS) {
        // `struct_info.fields` is parent-prefixed, so cleaning the whole span
        // destroyed an inherited `uniq` here *and* again in the parent the
        // destructor chains to — a double free (the interpreter's assert caught
        // it in debug; release would not have). Each level cleans only its own
        // fields.
        const char* source = R"(
        struct Res { v: i32; }
        fun delete Res() { print("res freed"); }

        struct Parent { r: uniq Res; }        // owned field -> synthesized dtor
        struct Child : Parent { c: i32; }

        fun new Child() { self.r = uniq Res { v = 1 }; self.c = 2; }
        fun delete Child() { print("child"); }

        fun main(): i32 {
            var x: uniq Child = uniq Child();
            delete x;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "child\nres freed\n");
    }

    TEST_CASE_TEMPLATE("Destructor chaining skips a level with no destructor", Backend, RX_E2E_BACKENDS) {
        // A missing middle level must not break the chain: the grandparent's
        // destructor still runs. Skipping is sound because a level without a
        // default destructor has nothing to run — no user body, and no owned
        // fields (or one would have been synthesized).
        const char* source = R"(
        struct A { a: i32; }
        fun delete A() { print("A"); }

        struct B : A { b: i32; }          // no destructor

        struct C : B { c: i32; }
        fun new C() { self.a = 1; self.b = 2; self.c = 3; }
        fun delete C() { print("C"); }

        fun main(): i32 {
            var x: uniq C = uniq C();
            delete x;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "C\nA\n");
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

#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// RAII Tests - Implicit Destruction at Scope Exit
// ============================================================================

TEST_CASE("E2E - RAII: implicit delete at scope exit") {
    // uniq variable should be implicitly deleted when scope ends
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            return p.x;
            // p is implicitly deleted before return
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: implicit delete at function end (void)") {
    // uniq variable in a void function is cleaned up at the end
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun do_work(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 10;
            var result: i32 = p.x;
            return result;
            // p is implicitly deleted
        }

        fun main(): i32 {
            return do_work();
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
}

TEST_CASE("E2E - RAII: implicit delete in loop iteration") {
    // uniq allocated inside a loop body is deleted each iteration
    const char* source = R"(
        struct Counter {
            value: i32;
        }

        fun main(): i32 {
            var sum: i32 = 0;
            for (var i: i32 = 0; i < 3; i = i + 1) {
                var c: uniq Counter = uniq Counter();
                c.value = i * 10;
                sum = sum + c.value;
                // c is implicitly deleted at end of loop body
            }
            print(f"{sum}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");  // 0 + 10 + 20
}

TEST_CASE("E2E - RAII: explicit delete + scope exit (no double-free)") {
    // Explicit delete marks variable as moved; scope exit doesn't double-delete
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            var result: i32 = p.x;
            delete p;
            // p is moved — scope exit won't delete again
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: return uniq (no delete of returned value)") {
    // Returning a uniq value moves it to the caller; it is NOT deleted
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun create(): uniq Point {
            var p: uniq Point = uniq Point();
            p.x = 99;
            p.y = 1;
            return p;  // p is moved out, NOT deleted
        }

        fun main(): i32 {
            var p: uniq Point = create();
            var result: i32 = p.x + p.y;
            // p is implicitly deleted at scope exit
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 100);
}

// ============================================================================
// RAII Tests - Move Semantics
// ============================================================================

TEST_CASE("E2E - RAII: move to function transfers ownership") {
    // Passing uniq to a function moves ownership; callee cleans up
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            var result: i32 = p.x + p.y;
            return result;
            // p is implicitly deleted
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 3;
            p.y = 7;
            var result: i32 = consume(p);
            // p is consumed — caller doesn't need to delete
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
}

// ============================================================================
// RAII Tests - Reassignment Auto-Delete
// ============================================================================

TEST_CASE("E2E - RAII: reassignment auto-deletes old value") {
    // Reassigning a uniq variable should delete the old value first
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 1;
            // Old point is auto-deleted before reassignment
            p = uniq Point();
            p.x = 42;
            return p.x;
            // p is implicitly deleted
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

// ============================================================================
// RAII Tests - Destructor Called on Implicit Delete
// ============================================================================

TEST_CASE("E2E - RAII: destructor called on implicit delete") {
    // Default destructor should be called when scope-exit implicit delete fires
    // We verify the destructor runs by having it print a marker
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource() {
            print(f"{"dtor"}");
        }

        fun use_resource(): i32 {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            var result: i32 = r.value;
            return result;
            // r is implicitly deleted, which calls ~Resource()
        }

        fun main(): i32 {
            var result: i32 = use_resource();
            print(f"{result}");
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Destructor prints "dtor", then main prints "42"
    CHECK(result.stdout_output == "dtor\n42\n");
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: destructor called in correct LIFO order") {
    // When multiple uniqs are in scope, destructors fire in reverse order
    // Verify by having each destructor print its name
    const char* source = R"(
        struct A {
            value: i32;
        }

        fun delete A() {
            print(f"{"~A"}");
        }

        struct B {
            value: i32;
        }

        fun delete B() {
            print(f"{"~B"}");
        }

        fun main(): i32 {
            var a: uniq A = uniq A();
            var b: uniq B = uniq B();
            // At scope exit: b destroyed first (LIFO), then a
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // b is declared second, so destroyed first (LIFO)
    CHECK(result.stdout_output == "~B\n~A\n");
}

// ============================================================================
// RAII Tests - Nil Safety
// ============================================================================

TEST_CASE("E2E - RAII: nil uniq is safely cleaned up") {
    // A nil uniq should not crash on implicit delete (DEL_OBJ on null is no-op)
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = nil;
            // p is nil — implicit delete at scope exit is a safe no-op
            return 42;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

// ============================================================================
// RAII Tests - Use-After-Move Compile Error
// ============================================================================

TEST_CASE("E2E - RAII: use-after-move compile error") {
    // Using a uniq variable after it's been moved should be a compile error
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            var result: i32 = consume(p);
            // p is moved — using it here should be a compile error
            return p.x;
        }
    )";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

TEST_CASE("E2E - RAII: use-after-delete compile error") {
    // Using a uniq variable after explicit delete should be a compile error
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            delete p;
            return p.x;  // Error: use of moved value
        }
    )";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

TEST_CASE("E2E - RAII: conditional move compile error") {
    // Moving in one branch of if/else but not other causes MaybeValid error
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            if (true) {
                var result: i32 = consume(p);
            }
            // p is possibly moved — using it here is an error
            return p.x;
        }
    )";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

// ============================================================================
// RAII Tests - Break/Continue Cleanup
// ============================================================================

TEST_CASE("E2E - RAII: break cleans up loop-scoped uniq") {
    // uniq allocated inside loop body should be cleaned up on break
    // (no crash or memory error means cleanup succeeded)
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            for (var i: i32 = 0; i < 5; i = i + 1) {
                var p: uniq Point = uniq Point();
                p.x = i;
                if (i == 2) {
                    break;
                    // p is cleaned up before break
                }
                // p is cleaned up at end of loop iteration
            }
            return 42;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: multiple uniqs in nested scopes") {
    // Test cleanup with multiple uniqs at different scope levels
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p1: uniq Point = uniq Point();
            p1.x = 1;
            {
                var p2: uniq Point = uniq Point();
                p2.x = 2;
                // p2 is cleaned up here (inner scope)
            }
            // p1 is still live
            return p1.x;
            // p1 is cleaned up before return
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

// ============================================================================
// RAII Tests - Named-Only Destructor Compile Errors
// ============================================================================

TEST_CASE("E2E - RAII: named-only destructor requires explicit delete") {
    // Struct with only named destructors (no default) left live at scope exit → compile error
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource.save_to(path: string) {
            print(f"saving to {path}");
        }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            return r.value;
            // r is live but has only named destructors → compile error
        }
    )";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

TEST_CASE("E2E - RAII: named-only destructor with explicit delete OK") {
    // Same struct but variable is explicitly deleted before scope exit → OK
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource.save_to(path: string) {
            print(f"saving to {path}");
        }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            var result: i32 = r.value;
            delete r.save_to("output.txt");
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "saving to output.txt\n");
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: default + named destructor uses implicit delete") {
    // Struct with both default and named destructors → RAII uses default, no error
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource() {
            print(f"{"default dtor"}");
        }

        fun delete Resource.save_to(path: string) {
            print(f"saving to {path}");
        }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.value = 42;
            return r.value;
            // r is implicitly deleted via default destructor
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "default dtor\n");
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: named-only destructor with break is compile error") {
    // Variable in loop body with only named destructors + break → compile error
    const char* source = R"(
        struct Resource {
            value: i32;
        }

        fun delete Resource.close() {
            print(f"{"closing"}");
        }

        fun main(): i32 {
            for (var i: i32 = 0; i < 5; i = i + 1) {
                var r: uniq Resource = uniq Resource();
                r.value = i;
                if (i == 2) {
                    break;
                    // r is live with only named destructors → compile error
                }
            }
            return 0;
        }
    )";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

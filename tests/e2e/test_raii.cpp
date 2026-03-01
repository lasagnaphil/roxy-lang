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

// ============================================================================
// Recursive Uniq Field Cleanup (Synthetic Destructors)
// ============================================================================

TEST_CASE("E2E - RAII: uniq field auto-cleanup at scope exit") {
    // When a struct with a uniq field goes out of scope,
    // the uniq field should be automatically deleted
    const char* source = R"(
        struct Inner {
            value: i32;
        }

        fun delete Inner() {
            print(f"{"~Inner"}");
        }

        struct Outer {
            child: uniq Inner;
        }

        fun main(): i32 {
            var o: uniq Outer = uniq Outer();
            o.child = uniq Inner();
            o.child.value = 42;
            var result: i32 = o.child.value;
            return result;
            // o is implicitly deleted, which should also delete o.child
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
    CHECK(result.stdout_output == "~Inner\n");
}

TEST_CASE("E2E - RAII: uniq field cleanup with explicit delete") {
    // Explicit delete of a struct with uniq fields should clean up the fields
    const char* source = R"CODE(
        struct Node {
            value: i32;
        }

        fun delete Node() {
            print(f"~Node({self.value})");
        }

        struct Container {
            item: uniq Node;
        }

        fun main(): i32 {
            var c: uniq Container = uniq Container();
            c.item = uniq Node();
            c.item.value = 99;
            delete c;
            return 0;
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "~Node(99)\n");
}

TEST_CASE("E2E - RAII: recursive uniq field cleanup (tree structure)") {
    // A tree with uniq children should recursively clean up all nodes
    const char* source = R"CODE(
        struct Node {
            value: i32;
            left: uniq Node;
            right: uniq Node;
        }

        fun delete Node() {
            print(f"~Node({self.value})");
        }

        fun main(): i32 {
            var root: uniq Node = uniq Node();
            root.value = 1;

            root.left = uniq Node();
            root.left.value = 2;

            root.right = uniq Node();
            root.right.value = 3;

            return 0;
            // root deleted → calls ~Node(1), then deletes right(3), left(2)
            // Exact order: user dtor runs first, then fields in reverse order
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Root's dtor runs first, then right field (reverse order), then left field
    // Each child's dtor runs before the child is deleted
    CHECK(result.stdout_output == "~Node(1)\n~Node(3)\n~Node(2)\n");
}

TEST_CASE("E2E - RAII: user-defined dtor + auto field cleanup") {
    // When a struct has BOTH a user-defined destructor AND uniq fields,
    // the user destructor runs first, then fields are cleaned up
    const char* source = R"CODE(
        struct Child {
            name: string;
        }

        fun delete Child() {
            print(f"~Child({self.name})");
        }

        struct Parent {
            child: uniq Child;
            label: string;
        }

        fun delete Parent() {
            print(f"~Parent({self.label})");
        }

        fun main(): i32 {
            var p: uniq Parent = uniq Parent();
            p.label = "root";
            p.child = uniq Child();
            p.child.name = "kid";
            return 0;
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Parent's user dtor runs first, then child field cleanup
    CHECK(result.stdout_output == "~Parent(root)\n~Child(kid)\n");
}

TEST_CASE("E2E - RAII: multiple uniq fields cleanup order") {
    // Multiple uniq fields should be cleaned up in reverse declaration order
    const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        struct Holder {
            first: uniq Item;
            second: uniq Item;
            third: uniq Item;
        }

        fun main(): i32 {
            var h: uniq Holder = uniq Holder();
            h.first = uniq Item();
            h.first.id = 1;
            h.second = uniq Item();
            h.second.id = 2;
            h.third = uniq Item();
            h.third.id = 3;
            return 0;
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Reverse declaration order: third, second, first
    CHECK(result.stdout_output == "~Item(3)\n~Item(2)\n~Item(1)\n");
}

TEST_CASE("E2E - RAII: uniq field without destructor (no struct inner type)") {
    // A struct with a uniq field pointing to a struct without any destructor
    // should still free the memory (no crash, no leak)
    const char* source = R"(
        struct Simple {
            value: i32;
        }

        struct Wrapper {
            item: uniq Simple;
        }

        fun main(): i32 {
            var w: uniq Wrapper = uniq Wrapper();
            w.item = uniq Simple();
            w.item.value = 42;
            var result: i32 = w.item.value;
            return result;
            // w is deleted, which should also delete w.item (no dtor, just free)
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - RAII: value-type struct field with destructor") {
    // When a struct embeds a value-type struct field whose type has a
    // destructor (due to owning uniq fields), the field's destructor
    // should be called automatically.
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"~Leaf({self.value})");
        }

        struct Inner {
            child: uniq Leaf;
        }

        // Inner gets a synthetic destructor because it has a uniq field.
        // Outer gets a synthetic destructor because it has a value-type
        // struct field (inner) whose type has a default destructor.

        struct Outer {
            inner: Inner;
        }

        fun main(): i32 {
            var o: uniq Outer = uniq Outer();
            o.inner.child = uniq Leaf();
            o.inner.child.value = 77;
            return 0;
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // Outer's synthetic dtor calls Inner's synthetic dtor on the embedded field,
    // which in turn cleans up the uniq Leaf child.
    CHECK(result.stdout_output == "~Leaf(77)\n");
}

TEST_CASE("E2E - RAII: deeply nested value-type fields with destructors") {
    // Three levels of nesting: C embeds B embeds A, where A has a uniq field.
    // All three should get synthetic destructors via the fixpoint loop.
    const char* source = R"CODE(
        struct Resource {
            id: i32;
        }

        fun delete Resource() {
            print(f"~Resource({self.id})");
        }

        struct A {
            res: uniq Resource;
        }

        struct B {
            a: A;
        }

        struct C {
            b: B;
        }

        fun main(): i32 {
            var c: uniq C = uniq C();
            c.b.a.res = uniq Resource();
            c.b.a.res.id = 42;
            return 0;
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "~Resource(42)\n");
}

// ============================================================================
// Value-Type Struct Move Semantics
// ============================================================================

TEST_CASE("E2E - RAII: value struct scope-exit cleanup") {
    // A value-type struct with a uniq field gets a synthetic destructor,
    // so it should be cleaned up at scope exit
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"~Leaf({self.value})");
        }

        struct Owner {
            child: uniq Leaf;
        }

        fun main(): i32 {
            var o: Owner = Owner();
            o.child = uniq Leaf();
            o.child.value = 55;
            return 0;
            // o is a value struct with synthetic destructor — cleaned up at scope exit
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "~Leaf(55)\n");
}

TEST_CASE("E2E - RAII: value struct use-after-move compile error") {
    // Passing a move-semantic value struct to a function moves it;
    // using it after is a compile error
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        struct Owner {
            child: uniq Leaf;
        }

        fun consume(o: Owner): i32 {
            return 0;
        }

        fun main(): i32 {
            var o: Owner = Owner();
            o.child = uniq Leaf();
            o.child.value = 1;
            var r: i32 = consume(o);
            // o is moved — using it here should be a compile error
            return o.child.value;
        }
    )CODE";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

TEST_CASE("E2E - RAII: value struct move on return") {
    // Returning a value struct with move semantics moves it to the caller;
    // no double-destroy should occur
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"~Leaf({self.value})");
        }

        struct Owner {
            child: uniq Leaf;
        }

        fun create(): Owner {
            var o: Owner = Owner();
            o.child = uniq Leaf();
            o.child.value = 77;
            return o;  // o is moved out, NOT destroyed
        }

        fun main(): i32 {
            var o: Owner = create();
            var result: i32 = o.child.value;
            print(f"{result}");
            return 0;
            // o is destroyed here (once, not twice)
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "77\n~Leaf(77)\n");
}

TEST_CASE("E2E - RAII: value struct reassignment destroys old") {
    // Reassigning a move-semantic value struct should destroy the old value first
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"~Leaf({self.value})");
        }

        struct Owner {
            child: uniq Leaf;
        }

        fun main(): i32 {
            var o: Owner = Owner();
            o.child = uniq Leaf();
            o.child.value = 1;
            // Old Owner is destroyed on reassignment
            o = Owner();
            o.child = uniq Leaf();
            o.child.value = 2;
            return 0;
            // New o destroyed at scope exit
        }
    )CODE";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "~Leaf(1)\n~Leaf(2)\n");
}

TEST_CASE("E2E - RAII: value struct conditional move compile error") {
    // Moving in one if-branch but using after → compile error
    const char* source = R"CODE(
        struct Leaf {
            value: i32;
        }

        struct Owner {
            child: uniq Leaf;
        }

        fun consume(o: Owner): i32 {
            return 0;
        }

        fun main(): i32 {
            var o: Owner = Owner();
            o.child = uniq Leaf();
            if (true) {
                var r: i32 = consume(o);
            }
            // o is possibly moved — using it is an error
            return o.child.value;
        }
    )CODE";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

TEST_CASE("E2E - RAII: plain struct remains copyable") {
    // A struct without uniq fields or destructors should be freely copyable
    const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun use_point(p: Point): i32 {
            return p.x + p.y;
        }

        fun main(): i32 {
            var p: Point = Point();
            p.x = 1;
            p.y = 2;
            var sum: i32 = use_point(p);
            // p is NOT moved — still usable
            var result: i32 = p.x + sum;
            return result;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 4);  // p.x (1) + sum (3) = 4
}

TEST_CASE("E2E - RAII: user-defined destructor makes struct non-copyable") {
    // A struct with a user-defined default destructor should require move semantics
    const char* source = R"CODE(
        struct File {
            fd: i32;
        }

        fun delete File() {
            print(f"closing fd {self.fd}");
        }

        fun consume(f: File): i32 {
            return f.fd;
        }

        fun main(): i32 {
            var f: File = File();
            f.fd = 42;
            var result: i32 = consume(f);
            // f is moved — using it here should be a compile error
            return f.fd;
        }
    )CODE";

    BumpAllocator allocator(65536);
    BCModule* module = compile(allocator, source);
    CHECK(module == nullptr);  // Should fail to compile
}

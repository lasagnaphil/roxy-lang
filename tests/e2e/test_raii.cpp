#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// RAII Tests - Implicit Destruction at Scope Exit
// ============================================================================

TEST_SUITE("E2E RAII") {

    TEST_CASE_TEMPLATE("implicit delete at scope exit", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("implicit delete at function end (void)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
    }

    TEST_CASE_TEMPLATE("implicit delete in loop iteration", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");  // 0 + 10 + 20
    }

    TEST_CASE_TEMPLATE("explicit delete + scope exit (no double-free)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("return uniq (no delete of returned value)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 100);
    }

    // ============================================================================
    // RAII Tests - Move Semantics
    // ============================================================================

    TEST_CASE_TEMPLATE("move to function transfers ownership", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
    }

    // ============================================================================
    // RAII Tests - Reassignment Auto-Delete
    // ============================================================================

    TEST_CASE_TEMPLATE("reassignment auto-deletes old value", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // RAII Tests - Destructor Called on Implicit Delete
    // ============================================================================

    TEST_CASE_TEMPLATE("destructor called on implicit delete", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // Destructor prints "dtor", then main prints "42"
        CHECK(result.stdout_output == "dtor\n42\n");
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("destructor called in correct LIFO order", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        // b is declared second, so destroyed first (LIFO)
        CHECK(result.stdout_output == "~B\n~A\n");
    }

    // ============================================================================
    // RAII Tests - Nil Safety
    // ============================================================================

    TEST_CASE_TEMPLATE("nil uniq is safely cleaned up", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // RAII Tests - Use-After-Move Compile Error
    // ============================================================================

    TEST_CASE("use-after-move compile error") {
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

    TEST_CASE("use-after-delete compile error") {
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

    TEST_CASE("self-assignment of noncopyable compile error") {
        // `x = x` on a uniq variable would delete the old value and then move the
        // now-dangling pointer back into x — a guaranteed use-after-free. The
        // semantic analyzer should reject it.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            p = p;  // Error: self-assignment of noncopyable
            return p.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE_TEMPLATE("cross-variable move still compiles", Backend, RX_E2E_BACKENDS) {
        // Regression guard: the self-assignment check must only fire when source
        // and target resolve to the same symbol. Moving between distinct uniq
        // variables is a normal move and must still compile.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var a: uniq Point = uniq Point();
            a.x = 7;
            var b: uniq Point = uniq Point();
            b = a;  // move a into b; a is consumed
            return b.x;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE("conditional move compile error") {
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

    TEST_CASE_TEMPLATE("break cleans up loop-scoped uniq", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("multiple uniqs in nested scopes", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    // ============================================================================
    // RAII Tests - Named-Only Destructor Compile Errors
    // ============================================================================

    TEST_CASE("named-only destructor requires explicit delete") {
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

    TEST_CASE_TEMPLATE("named-only destructor with explicit delete OK", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "saving to output.txt\n");
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("default + named destructor uses implicit delete", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "default dtor\n");
        CHECK(result.value == 42);
    }

    TEST_CASE("named-only destructor with break is compile error") {
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

    TEST_CASE("uniq field auto-cleanup at scope exit") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
        CHECK(result.stdout_output == "~Inner\n");
    }

    TEST_CASE("uniq field cleanup with explicit delete") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Node(99)\n");
    }

    TEST_CASE("recursive uniq field cleanup (tree structure)") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // Root's dtor runs first, then right field (reverse order), then left field
        // Each child's dtor runs before the child is deleted
        CHECK(result.stdout_output == "~Node(1)\n~Node(3)\n~Node(2)\n");
    }

    TEST_CASE("user-defined dtor + auto field cleanup") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // Parent's user dtor runs first, then child field cleanup
        CHECK(result.stdout_output == "~Parent(root)\n~Child(kid)\n");
    }

    TEST_CASE("multiple uniq fields cleanup order") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // Reverse declaration order: third, second, first
        CHECK(result.stdout_output == "~Item(3)\n~Item(2)\n~Item(1)\n");
    }

    TEST_CASE_TEMPLATE("uniq field without destructor (no struct inner type)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("value-type struct field with destructor") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // Outer's synthetic dtor calls Inner's synthetic dtor on the embedded field,
        // which in turn cleans up the uniq Leaf child.
        CHECK(result.stdout_output == "~Leaf(77)\n");
    }

    TEST_CASE("deeply nested value-type fields with destructors") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Resource(42)\n");
    }

    // ============================================================================
    // Value-Type Struct Move Semantics
    // ============================================================================

    TEST_CASE("value struct scope-exit cleanup") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Leaf(55)\n");
    }

    TEST_CASE("value struct use-after-move compile error") {
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

    TEST_CASE("value struct move on return") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "77\n~Leaf(77)\n");
    }

    TEST_CASE("value struct reassignment destroys old") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Leaf(1)\n~Leaf(2)\n");
    }

    TEST_CASE("value struct conditional move compile error") {
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

    TEST_CASE_TEMPLATE("plain struct remains copyable", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4);  // p.x (1) + sum (3) = 4
    }

    TEST_CASE("user-defined destructor makes struct non-copyable") {
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

    // ============================================================================
    // Field-Level Move Prevention (Compile Errors)
    // ============================================================================

    TEST_CASE("cannot pass uniq field to uniq parameter") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun take(item: uniq Item): i32 {
            return item.value;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            o.child.value = 42;
            return take(o.child);
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot move out of field
    }

    TEST_CASE("cannot assign uniq field to uniq variable") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            var x: uniq Item = o.child;
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot move out of field
    }

    TEST_CASE("cannot assign uniq field to inferred uniq variable") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            var x = o.child;
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot move out of field
    }

    TEST_CASE("cannot delete uniq field directly") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            delete o.child;
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot delete a field
    }

    TEST_CASE("cannot return uniq field") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun get_child(o: ref Owner): uniq Item {
            return o.child;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot return a field with move semantics
    }

    TEST_CASE("cannot reassign uniq variable from uniq field") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var x: uniq Item = uniq Item();
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            x = o.child;
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Cannot move out of field
    }

    // ============================================================================
    // Field Borrowing Still Works
    // ============================================================================

    TEST_CASE_TEMPLATE("reading through uniq field works", Backend, RX_E2E_BACKENDS) {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            o.child.value = 42;
            return o.child.value;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("passing uniq field as ref parameter works") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun use_ref(item: ref Item): i32 {
            return item.value;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            o.child.value = 42;
            return use_ref(o.child);
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("calling method on uniq field works", Backend, RX_E2E_BACKENDS) {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        fun Item.get_value(): i32 {
            return self.value;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            o.child.value = 42;
            return o.child.get_value();
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("reassigning uniq field with new value works") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        fun delete Item() {
            print(f"~Item({self.value})");
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var o: uniq Owner = uniq Owner();
            o.child = uniq Item();
            o.child.value = 1;
            o.child = uniq Item();
            o.child.value = 2;
            return o.child.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
        // First child (value=1) destroyed on reassignment, second (value=2) destroyed at scope exit
        CHECK(result.stdout_output == "~Item(1)\n~Item(2)\n");
    }

    // ============================================================================
    // Field Reassignment Cleanup (Runtime)
    // ============================================================================

    TEST_CASE("field reassignment destroys old uniq value") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        struct Container {
            item: uniq Item;
        }

        fun main(): i32 {
            var c: uniq Container = uniq Container();
            c.item = uniq Item();
            c.item.id = 1;
            print(f"{"before reassign"}");
            c.item = uniq Item();
            c.item.id = 2;
            print(f"{"after reassign"}");
            return 0;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // Old item destroyed during reassignment, new item destroyed at scope exit
        CHECK(result.stdout_output == "before reassign\n~Item(1)\nafter reassign\n~Item(2)\n");
    }

    TEST_CASE("field reassignment with nil destroys old value") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        struct Container {
            item: uniq Item;
        }

        fun main(): i32 {
            var c: uniq Container = uniq Container();
            c.item = uniq Item();
            c.item.id = 99;
            print(f"{"before nil"}");
            c.item = nil;
            print(f"{"after nil"}");
            return 0;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "before nil\n~Item(99)\nafter nil\n");
    }

    TEST_CASE("field assignment from uniq variable moves source") {
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var item: uniq Item = uniq Item();
            item.value = 42;
            var o: uniq Owner = uniq Owner();
            o.child = item;
            // item is now moved — using it should be a compile error
            return item.value;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // item is moved after field assignment
    }

    TEST_CASE("field assignment from uniq variable works correctly") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"CODE(
        struct Item {
            value: i32;
        }

        fun delete Item() {
            print(f"~Item({self.value})");
        }

        struct Owner {
            child: uniq Item;
        }

        fun main(): i32 {
            var item: uniq Item = uniq Item();
            item.value = 42;
            var o: uniq Owner = uniq Owner();
            o.child = item;
            var result: i32 = o.child.value;
            return result;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
        // Item destroyed once via Owner's field cleanup
        CHECK(result.stdout_output == "~Item(42)\n");
    }

    // ============================================================================
    // Noncopyable Container Tests — List<uniq T>
    // ============================================================================

    TEST_CASE_TEMPLATE("List<uniq T> empty list cleanup", Backend, RX_E2E_BACKENDS) {
        // Empty noncopyable list should be cleaned up without issues
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            print(f"created");
            return 0;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "created\n");
    }

    TEST_CASE_TEMPLATE("List<uniq T> element cleanup at scope exit", Backend, RX_E2E_BACKENDS) {
        // Elements in a List<uniq T> should be destroyed when the list goes out of scope
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            var a: uniq Item = uniq Item();
            a.id = 1;
            items.push(a);
            var b: uniq Item = uniq Item();
            b.id = 2;
            items.push(b);
            var c: uniq Item = uniq Item();
            c.id = 3;
            items.push(c);
            print(f"before exit");
            return 0;
            // items goes out of scope — all 3 elements should be destroyed
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "before exit\n~Item(1)\n~Item(2)\n~Item(3)\n");
    }

    TEST_CASE_TEMPLATE("List<uniq T> passed to function by move", Backend, RX_E2E_BACKENDS) {
        // Passing List<uniq T> to a function moves it; elements destroyed in callee
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        fun consume(items: List<uniq Item>): i32 {
            var result: i32 = items.len();
            return result;
            // items destroyed here with all its elements
        }

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            var a: uniq Item = uniq Item();
            a.id = 10;
            items.push(a);
            var b: uniq Item = uniq Item();
            b.id = 20;
            items.push(b);
            var result: i32 = consume(items);
            print(f"{result}");
            return 0;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Item(10)\n~Item(20)\n2\n");
    }

    TEST_CASE_TEMPLATE("struct with List<uniq T> field cleanup", Backend, RX_E2E_BACKENDS) {
        // A struct containing a List<uniq T> field should get a synthetic destructor
        // that cleans up the list elements
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        struct Container {
            items: List<uniq Item>;
        }

        fun main(): i32 {
            var c: uniq Container = uniq Container();
            c.items = List<uniq Item>();
            var a: uniq Item = uniq Item();
            a.id = 100;
            c.items.push(a);
            var b: uniq Item = uniq Item();
            b.id = 200;
            c.items.push(b);
            return 0;
            // c goes out of scope → synthetic destructor cleans up items list → elements destroyed
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "~Item(100)\n~Item(200)\n");
    }

    TEST_CASE("use-after-move on List<uniq T> via var init") {
        // Initializing a new variable from a noncopyable list moves it;
        // using the source after is a compile error
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {}

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            var copy = items;
            // items is moved — using it here should be a compile error
            return items.len();
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile — use after move
    }

    TEST_CASE("use-after-move on List<uniq T>") {
        // Using a List<uniq T> after passing it to a function should fail
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {}

        fun consume(items: List<uniq Item>): i32 {
            return items.len();
        }

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            var r: i32 = consume(items);
            // items is moved — using it here should be a compile error
            return items.len();
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile — use after move
    }

    TEST_CASE_TEMPLATE("List<i32> remains copyable", Backend, RX_E2E_BACKENDS) {
        // Regular lists with copyable element types are still copyable
        const char* source = R"CODE(
        fun use_list(items: List<i32>): i32 {
            return items.len();
        }

        fun main(): i32 {
            var items: List<i32> = List<i32>();
            items.push(1);
            items.push(2);
            var len1: i32 = use_list(items);
            // items is NOT moved — still usable
            var len2: i32 = items.len();
            return len1 + len2;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 4);  // 2 + 2
    }

    TEST_CASE_TEMPLATE("method calls on noncopyable list work", Backend, RX_E2E_BACKENDS) {
        // .push(), .len(), .pop() should still work on List<uniq T>
        const char* source = R"CODE(
        struct Item {
            id: i32;
        }

        fun delete Item() {
            print(f"~Item({self.id})");
        }

        fun main(): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            var a: uniq Item = uniq Item();
            a.id = 1;
            items.push(a);
            var b: uniq Item = uniq Item();
            b.id = 2;
            items.push(b);
            var len: i32 = items.len();
            print(f"len={len}");
            return 0;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "len=2\n~Item(1)\n~Item(2)\n");
    }

    // ============================================================================
    // Variable-to-variable move semantics
    // ============================================================================

    TEST_CASE_TEMPLATE("variable-to-variable move marks source as moved", Backend, RX_E2E_BACKENDS) {
        // After `b = a`, using `a` should be a compile error (use-after-move)
        const char* source = R"CODE(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var a: uniq Node = uniq Node();
            a.value = 10;
            var b: uniq Node = uniq Node();
            b = a;
            return a.value;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(!result.success);  // Should fail: use-after-move on `a`
    }

    TEST_CASE_TEMPLATE("variable-to-variable move works correctly", Backend, RX_E2E_BACKENDS) {
        // After `b = a`, using `b` should work fine
        const char* source = R"CODE(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var a: uniq Node = uniq Node();
            a.value = 42;
            var b: uniq Node = uniq Node();
            b = a;
            return b.value;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("linked list building in while loop") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Move head into field, reassign head, repeat — should work
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var head: uniq Node = uniq Node();
            head.value = 0;
            head.next = nil;

            var i: i32 = 1;
            while (i <= 3) {
                var new_node: uniq Node = uniq Node();
                new_node.value = i;
                new_node.next = head;
                head = new_node;
                i = i + 1;
            }

            var result: i32 = head.value;
            return result;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE("linked list building in for loop") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Same pattern with for loop
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var head: uniq Node = uniq Node();
            head.value = 0;
            head.next = nil;

            for (var i: i32 = 1; i <= 3; i = i + 1) {
                var new_node: uniq Node = uniq Node();
                new_node.value = i;
                new_node.next = head;
                head = new_node;
            }

            var result: i32 = head.value;
            return result;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE_TEMPLATE("move in loop without reassignment is error", Backend, RX_E2E_BACKENDS) {
        // Moving a uniq in a loop body without reassigning should be a compile error
        const char* source = R"CODE(
        struct Node {
            value: i32;
            child: uniq Node;
        }

        fun take_ownership(n: uniq Node): i32 {
            return n.value;
        }

        fun main(): i32 {
            var node: uniq Node = uniq Node();
            node.value = 5;

            var i: i32 = 0;
            while (i < 2) {
                take_ownership(node);
                i = i + 1;
            }
            return 0;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(!result.success);  // Should fail: use-after-move on next iteration
    }

    TEST_CASE_TEMPLATE("conditional move in loop is error", Backend, RX_E2E_BACKENDS) {
        // Moving in one branch of an if inside a loop — MaybeValid cross-iteration
        const char* source = R"CODE(
        struct Node {
            value: i32;
            child: uniq Node;
        }

        fun take_ownership(n: uniq Node): i32 {
            return n.value;
        }

        fun main(): i32 {
            var node: uniq Node = uniq Node();
            node.value = 5;

            var i: i32 = 0;
            while (i < 4) {
                if (i == 2) {
                    take_ownership(node);
                }
                i = i + 1;
            }
            return 0;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(!result.success);  // Should fail: MaybeValid cross-iteration
    }

    // ============================================================================
    // Deep else-if chain compilation performance
    // ============================================================================

    TEST_CASE_TEMPLATE("deep else-if chain with noncopyable types", Backend, RX_E2E_BACKENDS) {
        // Regression: 20+ else-if branches with List<uniq T> in scope
        // previously caused the compiler to hang (quadratic compilation)
        const char* source = R"CODE(
        struct Item { value: i32; }

        fun classify(n: i32): i32 {
            var items: List<uniq Item> = List<uniq Item>();
            if (n == 1) { items.push(uniq Item()); items[0].value = 10; }
            else if (n == 2) { items.push(uniq Item()); items[0].value = 20; }
            else if (n == 3) { items.push(uniq Item()); items[0].value = 30; }
            else if (n == 4) { items.push(uniq Item()); items[0].value = 40; }
            else if (n == 5) { items.push(uniq Item()); items[0].value = 50; }
            else if (n == 6) { items.push(uniq Item()); items[0].value = 60; }
            else if (n == 7) { items.push(uniq Item()); items[0].value = 70; }
            else if (n == 8) { items.push(uniq Item()); items[0].value = 80; }
            else if (n == 9) { items.push(uniq Item()); items[0].value = 90; }
            else if (n == 10) { items.push(uniq Item()); items[0].value = 100; }
            else if (n == 11) { items.push(uniq Item()); items[0].value = 110; }
            else if (n == 12) { items.push(uniq Item()); items[0].value = 120; }
            else if (n == 13) { items.push(uniq Item()); items[0].value = 130; }
            else if (n == 14) { items.push(uniq Item()); items[0].value = 140; }
            else if (n == 15) { items.push(uniq Item()); items[0].value = 150; }
            else if (n == 16) { items.push(uniq Item()); items[0].value = 160; }
            else if (n == 17) { items.push(uniq Item()); items[0].value = 170; }
            else if (n == 18) { items.push(uniq Item()); items[0].value = 180; }
            else if (n == 19) { items.push(uniq Item()); items[0].value = 190; }
            else { items.push(uniq Item()); items[0].value = 0; }
            var result: i32 = 0;
            if (items.len() > 0) { result = items[0].value; }
            return result;
        }

        fun main(): i32 {
            return classify(5) + classify(15);
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 200);  // 50 + 150
    }

    TEST_CASE_TEMPLATE("variable shadowing does not corrupt outer move state", Backend, RX_E2E_BACKENDS) {
        // After moving outer x, an inner scope declaring a new x should not
        // make the outer x appear live again after the inner scope exits.
        const char* source = R"CODE(
        struct Widget {
            id: i32;
        }

        fun delete Widget() { }

        fun consume(w: uniq Widget) { }

        fun main(): i32 {
            var x: uniq Widget = uniq Widget { id = 1 };
            consume(x);
            {
                var x: uniq Widget = uniq Widget { id = 2 };
            }
            return x.id;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(!result.success);  // Should fail: use of moved value 'x' (outer)
    }

    TEST_CASE_TEMPLATE("struct literal field marks source as moved", Backend, RX_E2E_BACKENDS) {
        SUBCASE("Use after move into struct literal is rejected") {
            const char* source = R"CODE(
            struct Widget {
                id: i32;
            }

            fun delete Widget() { }

            struct Wrapper {
                item: uniq Widget;
            }

            fun main(): i32 {
                var w: uniq Widget = uniq Widget { id = 42 };
                var wrapper: Wrapper = Wrapper { item = w };
                return w.id;
            }
        )CODE";

            auto result = Backend::run(source);
            CHECK(!result.success);  // Should fail: use of moved value 'w'
        }

        SUBCASE("Move into struct literal without later use succeeds") {
            const char* source = R"CODE(
            struct Widget {
                id: i32;
            }

            fun delete Widget() { }

            struct Wrapper {
                item: uniq Widget;
            }

            fun main(): i32 {
                var w: uniq Widget = uniq Widget { id = 42 };
                var wrapper: Wrapper = Wrapper { item = w };
                return wrapper.item.id;
            }
        )CODE";

            auto result = Backend::run(source);
            CHECK(result.success);
            CHECK(result.value == 42);
        }
    }

    // ============================================================================
    // RAII Tests - Definite-termination analysis
    // ============================================================================
    // If one branch of an if/when/try always returns/throws/breaks/continues, the
    // merge should pick the surviving branch's move state instead of MaybeValid.

    TEST_CASE_TEMPLATE("early return in if-then preserves x live after", Backend, RX_E2E_BACKENDS) {
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
            if (false) {
                return 0;
            }
            // p is still live here — then-branch terminated
            return consume(p);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("throw in if-then preserves x live after", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct BadInput {
            code: i32;
        }

        fun BadInput.message(): string for Exception {
            return "bad input";
        }

        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun do_work(bail: bool): i32 {
            var p: uniq Point = uniq Point();
            p.x = 99;
            if (bail) {
                throw BadInput { code = 1 };
            }
            return consume(p);
        }

        fun main(): i32 {
            try {
                return do_work(false);
            } catch (e: BadInput) {
                return -1;
            }
            return -2;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    TEST_CASE_TEMPLATE("move and return in then, live in else", Backend, RX_E2E_BACKENDS) {
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
            p.x = 7;
            if (false) {
                return consume(p);
            } else {
                // else keeps p live; then moved and returned
            }
            // p is live here — only else-branch survives
            return consume(p);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("when with one terminating case preserves live path", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        enum Kind { A, B }

        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 5;
            var k: Kind = Kind::B;
            when k {
                case A:
                    return 0;
                case B:
                    // fall through, p still live
            }
            return consume(p);
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 5);
    }

    TEST_CASE("try body move with terminating catch keeps x moved after") {
        // Regression/precision test: catch terminates, so the only surviving
        // exit path is the try body where x was moved. Using x after must error.
        const char* source = R"(
        struct E { code: i32; }
        fun E.message(): string for Exception { return "e"; }

        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            try {
                var r: i32 = consume(p);
            } catch (e: E) {
                return -1;
            }
            // p is moved on the surviving try-exit path
            return p.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE_TEMPLATE("reassignment in nested scope adopts RHS temporary", Backend, RX_E2E_BACKENDS) {
        // Regression for an IR-gen fix: gen_assign_expr must consume the RHS
        // temporary when the target is a noncopyable identifier, otherwise the
        // temp's cleanup record at the inner scope depth triggers a double-free
        // against the outer variable's cleanup when the scopes pop in order.
        // This is observable when the reassignment sits in a nested block that
        // pops before the variable's scope.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 1;
            if (true) {
                p = uniq Point();
                p.x = 42;
            }
            return p.x;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("reassignment in catch after terminating try", Backend, RX_E2E_BACKENDS) {
        // Exercises both the try/catch termination analysis and the
        // gen_assign_expr temp-adoption fix: the try throws (terminating), so
        // only the catch path survives; the catch reassigns a uniq variable
        // from an outer scope, and the value must flow out to the caller
        // without double-destroying the new resource.
        const char* source = R"(
        struct Resource { id: i32; }
        fun delete Resource() { }

        struct E { code: i32; }
        fun E.message(): string for Exception { return "e"; }

        fun main(): i32 {
            var r: uniq Resource = uniq Resource();
            r.id = 1;
            try {
                throw E { code = 0 };
            } catch (e: E) {
                r = uniq Resource();
                r.id = 7;
            }
            return r.id;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE("loop body return does not escape enclosing if") {
        // Regression: if we incorrectly propagated termination from a loop body,
        // we'd accept this program. The loop may execute zero times, so the
        // then-branch may fall through with p moved — use of p after must still
        // be rejected.
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
            if (true) {
                var r: i32 = consume(p);  // moves p
                while (false) {
                    return 0;
                }
                // loop may run 0 times; p is moved on this path
            }
            return p.x;  // should error: p may have been moved
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    // ============================================================================
    // RAII Tests - Ternary move-state merging
    // ============================================================================
    // Ternary branches are evaluated conditionally at runtime, so the analyzer
    // must save/restore state across the branches and merge results — same
    // protocol used by if/else. A naive linear analysis would either produce
    // spurious use-after-move errors in the second branch, or miss conditional
    // moves that should mark a variable MaybeValid after the expression.

    TEST_CASE("ternary consumes different variables in each branch") {
        // Each branch moves a different uniq variable. After the ternary, both
        // should be MaybeValid; using either must be a compile error.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun main(): i32 {
            var a: uniq Point = uniq Point();
            var b: uniq Point = uniq Point();
            var r: i32 = true ? consume(a) : consume(b);
            return a.x;  // error: a is MaybeValid
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE("ternary consumes same variable in both branches") {
        // Both branches move the same variable — post-ternary it is definitely
        // Moved; use after must be a compile error.
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
            var r: i32 = true ? consume(p) : consume(p);
            return p.x;  // error: p is Moved
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE("ternary consumes in only one branch is MaybeValid") {
        // Without merge, the linear analysis would mark p as Moved after the
        // ternary; with merge it is MaybeValid (one branch consumes, other
        // does not). Either way, a post-ternary use must be rejected.
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
            var r: i32 = true ? consume(p) : 0;
            return p.x;  // error: p is MaybeValid
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE_TEMPLATE("ternary with no moves keeps variable live", Backend, RX_E2E_BACKENDS) {
        // Regression guard: a ternary over pure int branches must not affect
        // the move state of uniq variables in scope.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 5;
            var r: i32 = true ? 1 : 2;
            return p.x + r;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    // ============================================================================
    // IR-builder companion to definite-termination merging:
    // the IR builder must roll back local-scope and is_moved bookkeeping when a
    // branch terminates, so the merge-block code (reachable only via surviving
    // paths) doesn't see the consumed-and-nullified locals from the dead branch.
    // ============================================================================

    TEST_CASE("terminating then-branch struct-literal move keeps local live for after-if struct literal") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Pre-fix: the then-branch struct literal nullify-replaced `cond` to nil;
        // the after-if path then embedded nil into its own struct literal and
        // segfaulted on the field dereference at main.
        const char* source = R"ROXY(
        enum NodeKind { NLeaf, NIf }

        struct Node {
            when kind: NodeKind {
                case NLeaf: pub leaf_val: i32;
                case NIf:   pub if_cond: uniq Node;
            }
        }

        fun build_leaf(v: i32): uniq Node {
            return uniq Node { kind = NodeKind::NLeaf, leaf_val = v };
        }

        fun build_if(has_else: bool): uniq Node {
            var cond: uniq Node = build_leaf(1);
            if (has_else) {
                return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            }
            return uniq Node { kind = NodeKind::NIf, if_cond = cond };
        }

        fun main(): i32 {
            var n: uniq Node = build_if(false);
            return n.if_cond.leaf_val;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE("terminating else-branch struct-literal move keeps local live for after-if struct literal") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Symmetric to the above: when the else-branch terminates, the merge block
        // is reachable only via the then-branch (which here also moves the local).
        // The IR builder must restore the post-then snapshot rather than carrying
        // the else-branch's nullify-replace into the merge.
        const char* source = R"ROXY(
        enum NodeKind { NLeaf, NIf }

        struct Node {
            when kind: NodeKind {
                case NLeaf: pub leaf_val: i32;
                case NIf:   pub if_cond: uniq Node;
            }
        }

        fun build_leaf(v: i32): uniq Node {
            return uniq Node { kind = NodeKind::NLeaf, leaf_val = v };
        }

        fun build_if(go_then: bool): uniq Node {
            var cond: uniq Node = build_leaf(2);
            if (go_then) {
                // then keeps cond live, just falls through to the after-if return
            } else {
                return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            }
            return uniq Node { kind = NodeKind::NIf, if_cond = cond };
        }

        fun main(): i32 {
            var n: uniq Node = build_if(true);
            return n.if_cond.leaf_val;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

    TEST_CASE("terminating then-branch with else preserves else's post-state at merge") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // then-branch terminates; the merge block is reachable only via the
        // else-branch. The IR builder should keep the else's post-state at merge
        // (not roll back to pre-if), so the after-if code sees the right values.
        const char* source = R"ROXY(
        enum NodeKind { NLeaf, NIf }

        struct Node {
            when kind: NodeKind {
                case NLeaf: pub leaf_val: i32;
                case NIf:   pub if_cond: uniq Node;
            }
        }

        fun build_leaf(v: i32): uniq Node {
            return uniq Node { kind = NodeKind::NLeaf, leaf_val = v };
        }

        fun build_if(go_then: bool): uniq Node {
            var cond: uniq Node = build_leaf(3);
            if (go_then) {
                return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            } else {
                // else falls through; the after-if return below consumes cond
            }
            return uniq Node { kind = NodeKind::NIf, if_cond = cond };
        }

        fun main(): i32 {
            var n: uniq Node = build_if(false);
            return n.if_cond.leaf_val;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE("terminating when-case struct-literal move keeps local live for after-when struct literal") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Same shape as the if-stmt termination tests, but for when. Pre-fix the
        // last case body's nullify-replace of `cond` to nil leaked into the
        // merge block, segfaulting on dereference of the after-when struct
        // literal's if_cond field.
        const char* source = R"ROXY(
        enum NodeKind { NLeaf, NIf }
        enum K { KA, KB }

        struct Node {
            when kind: NodeKind {
                case NLeaf: pub leaf_val: i32;
                case NIf:   pub if_cond: uniq Node;
            }
        }

        fun build_leaf(v: i32): uniq Node {
            return uniq Node { kind = NodeKind::NLeaf, leaf_val = v };
        }

        fun build_via_when(k: K): uniq Node {
            var cond: uniq Node = build_leaf(7);
            when k {
                case KA:
                    // no-op, falls through
                case KB:
                    return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            }
            return uniq Node { kind = NodeKind::NIf, if_cond = cond };
        }

        fun main(): i32 {
            var n: uniq Node = build_via_when(K::KA);
            return n.if_cond.leaf_val;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE("terminating else-if-chain branch struct-literal move keeps local live after chain") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Same shape but for an else-if cascade (gen_if_else_chain). The last
        // chain branch's nullify-replace of `cond` would otherwise leak into the
        // merge block.
        const char* source = R"ROXY(
        enum NodeKind { NLeaf, NIf }

        struct Node {
            when kind: NodeKind {
                case NLeaf: pub leaf_val: i32;
                case NIf:   pub if_cond: uniq Node;
            }
        }

        fun build_leaf(v: i32): uniq Node {
            return uniq Node { kind = NodeKind::NLeaf, leaf_val = v };
        }

        fun build_via_chain(which: i32): uniq Node {
            var cond: uniq Node = build_leaf(7);
            if (which == 1) {
                return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            } else if (which == 2) {
                return uniq Node { kind = NodeKind::NIf, if_cond = cond };
            }
            return uniq Node { kind = NodeKind::NIf, if_cond = cond };
        }

        fun main(): i32 {
            var n: uniq Node = build_via_chain(0);
            return n.if_cond.leaf_val;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    // ============================================================================
    // User-defined constructors with noncopyable fields must not destroy stale
    // pre-call stack bytes (regression: Holder() assigning self.stmts hit the
    // destroy-old preamble with whatever pointer was left in the caller's return
    // slot from a previous call, double-freeing the previous Holder's list).
    // ============================================================================

    TEST_CASE_TEMPLATE("Constructor: reassigning a List<uniq T> field survives sequential calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Stmt { val: i32; }

        pub struct Holder { pub stmts: List<uniq Stmt>; }
        fun new Holder() { self.stmts = List<uniq Stmt>(); }

        fun make_list(): List<uniq Stmt> {
            var lst: List<uniq Stmt> = List<uniq Stmt>();
            var s: uniq Stmt = uniq Stmt();
            s.val = 42;
            lst.push(s);
            return lst;
        }

        fun go(): i32 {
            var h: Holder = Holder();
            h.stmts = make_list();
            return h.stmts[0].val;
        }

        fun main(): i32 {
            var a: i32 = go();
            var b: i32 = go();
            return a + b;
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 84);
    }

    TEST_CASE_TEMPLATE("Constructor: uniq field initialisation survives sequential calls", Backend, RX_E2E_BACKENDS) {
        // Same pattern for a plain uniq-typed field (no container wrapping).
        const char* source = R"ROXY(
        struct Payload { val: i32; }
        pub struct Holder { pub p: uniq Payload; }

        fun new Holder() {
            self.p = uniq Payload();
            self.p.val = 0;
        }

        fun go(v: i32): i32 {
            var h: Holder = Holder();
            h.p = uniq Payload();
            h.p.val = v;
            return h.p.val;
        }

        fun main(): i32 {
            return go(3) + go(4);
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    // ============================================================================
    // Constructor calls must consume/nullify noncopyable identifier arguments
    // (regression: gen_constructor_call didn't move-nullify identifier args like
    // gen_call_expr does, so `uniq T.name(leaf)` left `leaf` still pointing at
    // the slot the constructor stored into a field, causing a double-free at
    // scope exit).
    // ============================================================================

    TEST_CASE("Constructor call consumes noncopyable identifier argument stored in variant field") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"ROXY(
        enum K { A, B }
        struct Node {
            expr_id: i32;
            when kind: K { case A: child: uniq Node; case B: val: i32; }
        }
        fun new Node.a(id: i32, c: uniq Node) {
            self.expr_id = id;
            self.kind = K::A;
            self.child = c;
        }
        fun new Node.b(id: i32, v: i32) {
            self.expr_id = id;
            self.kind = K::B;
            self.val = v;
        }
        fun main(): i32 {
            var leaf: uniq Node = uniq Node.b(1, 42);
            var parent: uniq Node = uniq Node.a(2, leaf);
            when parent.kind {
                case A: return parent.child.val;
                case B: return -1;
            }
            return -2;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("Constructor call consumes inline uniq rvalue argument stored in variant field") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Exercises the rvalue/temp path (not just identifiers): the inner
        // `uniq Node.b(...)` is an rvalue temporary passed to Node.a. It must be
        // consumed via the temp-nullify path, not the identifier path.
        const char* source = R"ROXY(
        enum K { A, B }
        struct Node {
            expr_id: i32;
            when kind: K { case A: child: uniq Node; case B: val: i32; }
        }
        fun new Node.a(id: i32, c: uniq Node) {
            self.expr_id = id;
            self.kind = K::A;
            self.child = c;
        }
        fun new Node.b(id: i32, v: i32) {
            self.expr_id = id;
            self.kind = K::B;
            self.val = v;
        }
        fun main(): i32 {
            var parent: uniq Node = uniq Node.a(2, uniq Node.b(1, 99));
            when parent.kind {
                case A: return parent.child.val;
                case B: return -1;
            }
            return -2;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    // ============================================================================
    // Synthesized default constructor must null-init variant uniq fields too — the
    // union region aliases with whatever bytes were in the caller's local_stack
    // from a previous stack-allocation; a later `self.variant_field = …` would
    // destroy that stale pointer and crash.
    // ============================================================================

    TEST_CASE("Synthesized default ctor null-inits variant uniq fields for stack-reuse safety") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        const char* source = R"ROXY(
        enum K { A, B }
        struct Node {
            when kind: K {
                case A: child: uniq Node;
                case B: val: i32;
            }
        }
        // pollute() leaves a real uniq Node pointer in the caller's local_stack
        // slot. When go() then stack-allocates its own Node via `Node()`
        // (synthesized default ctor), the union-region bytes overlap with the
        // polluted pointer. Without null-init, the next `n.child = leaf`
        // destroy-old preamble frees that stale pointer.
        fun pollute(): Node {
            return Node { kind = K::A, child = uniq Node { kind = K::B, val = 99 } };
        }
        fun go(): i32 {
            var n: Node = Node();
            var leaf: uniq Node = uniq Node { kind = K::B, val = 42 };
            n.child = leaf;
            return n.child.val;
        }
        fun main(): i32 {
            var p: Node = pollute();
            return go();
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    // ============================================================================
    // Moving a noncopyable field out of a by-value struct parameter must null
    // the source field so the parameter's scope-exit destructor doesn't double-
    // free it. Semantic analysis already permits the move; IR gen was copying
    // the pointer into the target without clearing the source.
    // ============================================================================

    TEST_CASE_TEMPLATE("Field move from by-value struct param nulls the source field", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Thing { pub items: List<uniq Thing>; }
        fun new Thing() { self.items = List<uniq Thing>(); }

        struct Holder { pub things: List<uniq Thing>; }
        fun new Holder(src: Thing) { self.things = src.items; }

        fun main(): i32 {
            var t: Thing = Thing();
            t.items.push(uniq Thing());
            t.items.push(uniq Thing());
            var h: Holder = Holder(t);
            return i32(h.things.len());
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

    TEST_CASE_TEMPLATE("Field move from by-value struct param preserves unrelated fields", Backend, RX_E2E_BACKENDS) {
        // The source struct may have other fields that were NOT moved; they still
        // belong to `src` and should be cleaned up by its destructor normally.
        const char* source = R"ROXY(
        struct Payload { pub items: List<uniq Payload>; }
        fun new Payload() { self.items = List<uniq Payload>(); }

        struct Bundle {
            pub moved: List<uniq Payload>;
            pub retained: List<uniq Payload>;
        }
        fun new Bundle() {
            self.moved = List<uniq Payload>();
            self.retained = List<uniq Payload>();
        }

        struct Target { pub owned: List<uniq Payload>; }
        fun new Target(src: Bundle) { self.owned = src.moved; }

        fun main(): i32 {
            var b: Bundle = Bundle();
            b.moved.push(uniq Payload());
            b.moved.push(uniq Payload());
            b.retained.push(uniq Payload());
            var target: Target = Target(b);
            // Target should own 2 (from moved); b.retained still has 1 and will
            // clean up correctly at scope exit.
            return i32(target.owned.len());
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

    // ============================================================================
    // Nested field moves through value-struct chains. `obj.inner.field` is allowed
    // when every link in the chain is a value struct (no references); the root
    // identifier is marked moved.
    // ============================================================================

    TEST_CASE_TEMPLATE("Nested field move through value-struct chain", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Payload { pub items: List<uniq Payload>; }
        fun new Payload() { self.items = List<uniq Payload>(); }

        struct Inner { pub items: List<uniq Payload>; }
        fun new Inner() { self.items = List<uniq Payload>(); }

        struct Outer { pub inner: Inner; }
        fun new Outer() { self.inner = Inner(); }

        struct Target { pub owned: List<uniq Payload>; }
        fun new Target(src: Outer) { self.owned = src.inner.items; }

        fun main(): i32 {
            var o: Outer = Outer();
            o.inner.items.push(uniq Payload());
            o.inner.items.push(uniq Payload());
            o.inner.items.push(uniq Payload());
            var target: Target = Target(o);
            return i32(target.owned.len());
        }
    )ROXY";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE("Nested field move rejected when chain crosses a uniq reference") {
        // A `uniq` link in the chain means the storage is owned through
        // indirection; moving out from behind the reference is forbidden.
        const char* source = R"ROXY(
        struct Item { pub val: i32; }
        struct Inner { pub items: List<uniq Item>; }
        fun new Inner() { self.items = List<uniq Item>(); }
        struct Outer { pub inner: uniq Inner; }
        fun new Outer() { self.inner = uniq Inner(); }

        fun main(): i32 {
            var o: Outer = Outer();
            var stolen: List<uniq Item> = o.inner.items;
            return 0;
        }
    )ROXY";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE("Nested field move from already-moved root is rejected") {
        // After moving a nested field, the root variable is marked moved. A
        // subsequent move from the same root must be flagged as use-after-move.
        const char* source = R"ROXY(
        struct Payload { pub items: List<uniq Payload>; }
        fun new Payload() { self.items = List<uniq Payload>(); }

        struct Inner { pub items: List<uniq Payload>; }
        fun new Inner() { self.items = List<uniq Payload>(); }

        struct Outer { pub a: Inner; pub b: Inner; }
        fun new Outer() { self.a = Inner(); self.b = Inner(); }

        fun main(): i32 {
            var o: Outer = Outer();
            var first: List<uniq Payload> = o.a.items;
            var second: List<uniq Payload> = o.b.items;
            return 0;
        }
    )ROXY";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    // ===== Diagnostic tests for suspected move-state holes =====
    // These document the *desired* behavior. If a CHECK fails, it confirms the
    // corresponding analysis hole is real.

    TEST_CASE("move through parenthesized identifier is tracked") {
        // consume_noncopyable() peels groupings before marking the source moved.
        // A move whose argument is wrapped in a grouping — consume((p)) — denotes
        // the same storage as consume(p) and must transfer ownership, so the
        // later use of p is a compile error (use of moved value). Without the
        // grouping unwrap, the move would launder past the identifier check and
        // leave p Live — a use-after-move false negative.
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
            var result: i32 = consume((p));  // move through grouping
            // p is moved — using it here should be a compile error
            return p.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("reassign-before-move in loop body is legal") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // p is Live before the loop, reassigned at the TOP of every iteration,
        // and only then moved. Each iteration refreshes p before any use, so
        // there is no cross-iteration use-after-move. The cross-iteration check
        // exempts variables whose first body statement is a plain reassignment
        // (loop_reassigns_var_first), so this compiles and runs, returning
        // 0 + 1 + 2 == 3.
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
            var i: i32 = 0;
            var total: i32 = 0;
            while (i < 3) {
                p = uniq Point();          // reassigned at top of every iteration
                p.x = i;
                total = total + consume(p); // then moved
                i = i + 1;
            }
            return total;
        }
    )";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 3);
    }

    TEST_CASE("move in loop body without reassignment is still rejected") {
        // Soundness guard for the reassign-before-move exemption: a variable that
        // is moved in the loop body and NOT refreshed by a leading reassignment
        // would be used-after-move on the next iteration, so it must still be a
        // compile error. (Here the move precedes the reassignment, so the leading
        // statement is the move, not a refresh.)
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun run(): i32 {
            var p: uniq Point = uniq Point();
            var i: i32 = 0;
            while (i < 3) {
                var r: i32 = consume(p);  // p moved, never reassigned in body
                i = i + 1;
            }
            return 0;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile (cross-iteration use-after-move)
    }

    TEST_CASE("move out of conditional over owned variables is rejected") {
        // `cond ? a : b` reads one operand's pointer and yields it WITHOUT
        // nullifying the source (unlike a normal move, which nulls the moved
        // variable so scope-exit DEL_OBJ is a safe no-op). The selected operand
        // therefore stays live and is destroyed a second time at scope exit —
        // a double-free. The current cleanup model can't conditionally null a
        // ternary operand, so this must be rejected at compile time.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun run(cond: bool): i32 {
            var a: uniq Point = uniq Point();
            a.x = 1;
            var b: uniq Point = uniq Point();
            b.x = 2;
            return consume(cond ? a : b);  // double-free hazard — must be rejected
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("move out of conditional over fresh temporaries is also rejected") {
        // Even when both ternary operands are fresh temporaries, the construct is
        // rejected: gen_ternary_expr merges the branch values through a phi but
        // does not reconcile the per-branch noncopyable temporaries with it, so
        // the taken branch's object is freed twice (its consumer + statement-end
        // cleanup). Until the IR builder tracks ternary temp ownership through the
        // phi, noncopyable ternaries are rejected wholesale rather than miscompiled.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun consume(p: uniq Point): i32 {
            return p.x;
        }

        fun run(cond: bool): i32 {
            return consume(cond ? uniq Point { x = 1, y = 0 }
                                : uniq Point { x = 2, y = 0 });
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("move out of noncopyable list element is rejected") {
        // `List<uniq T>.index` returns `borrowed T` == `ref T`, so binding the
        // result to a `uniq` owner is a plain ref->uniq type error (a borrow does
        // not transfer ownership). This replaces the earlier move-checker stopgap
        // for uniq elements — moving the element out would otherwise double-free
        // it (owner + the list's own cleanup).
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var list: List<uniq Point> = List<uniq Point>();
            list.push(uniq Point { x = 42, y = 0 });
            var x: uniq Point = list[0];   // ref Point -> uniq Point: type error
            return x.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("move out of noncopyable map value is rejected") {
        // Same hazard via map indexing: `index` returns `borrowed V` == `ref V`.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var m: Map<i32, uniq Point> = Map<i32, uniq Point>();
            m.insert(0, uniq Point { x = 42, y = 0 });
            var x: uniq Point = m[0];   // ref Point -> uniq Point: type error
            return x.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("move out of noncopyable Map.get() result is rejected") {
        // `get` returns `borrowed V` too, so the same hazard via the named
        // accessor (not the `[]` operator) is also a ref->uniq type error. The
        // ExprIndex-scoped move-checker guard would NOT catch this call form;
        // routing `get` through the shared `borrowed` conversion does.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var m: Map<i32, uniq Point> = Map<i32, uniq Point>();
            m.insert(0, uniq Point { x = 42, y = 0 });
            var x: uniq Point = m.get(0);   // ref Point -> uniq Point: type error
            return x.x;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE_TEMPLATE("borrowing and reading a noncopyable list element is allowed", Backend, RX_E2E_BACKENDS) {
        // Soundness guard against over-rejection: the index rejection fires only
        // when the noncopyable element is *moved* out. Borrowing it as `ref` and
        // reading a field through it transfer no ownership and must keep working.
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var list: List<uniq Point> = List<uniq Point>();
            list.push(uniq Point { x = 42, y = 8 });
            var b: ref Point = list[0];   // borrow
            return b.x + list[0].y;       // borrow + field read
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 50);
    }

    TEST_CASE("move out of a user-defined index result is allowed") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // The rejection keys off the `index` method being *native*. A user `index`
        // (here via the builtin `Index<Idx, Output>` trait) has a move-checked
        // body, so its noncopyable return is a genuine ownership transfer (a fresh
        // temporary) and consuming it is sound — it must NOT be rejected like the
        // built-in container case.
        const char* source = R"(
        struct Point { x: i32; y: i32; }
        struct Maker { base: i32; }
        fun Maker.index(i: i32): uniq Point for Index<i32, uniq Point> {
            return uniq Point { x = self.base + i, y = 0 };  // fresh temp, not an alias
        }
        fun main(): i32 {
            var m: Maker = Maker { base = 40 };
            var p: uniq Point = m[2];   // consume a move-checked method return
            return p.x;                 // 42
        }
    )";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("borrowed uniq T resolves to ref T (a borrow)", Backend, RX_E2E_BACKENDS) {
        // The `borrowed` type modifier demotes `uniq T` to `ref T`: the source is
        // borrowed, not moved, so `owner` stays usable afterward.
        const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun main(): i32 {
            var owner: uniq Point = uniq Point { x = 42, y = 0 };
            var b: borrowed uniq Point = owner;   // -> ref Point (borrow)
            return b.x + owner.x;                 // owner not moved: 42 + 42 == 84
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 84);
    }

    TEST_CASE_TEMPLATE("borrowed copyable T resolves to T (a copy)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var b: borrowed i32 = 5;   // copyable -> i32 (copy out)
            return b;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 5);
    }

    TEST_CASE("moving a pointer field out destroys it exactly once") {  // VM-only: C backend: uniq/RAII destructor + struct-value-move semantics gap
        // Moving a noncopyable pointer field (`o.a`) out of a value struct nulls
        // that field in the root, so the root's destructor no-ops it (frees only
        // the *sibling* `o.b`) instead of re-destroying the moved-out value. Each
        // Foo destructor must run exactly once; the bug was a double-destruction
        // of `o.a` (the root re-freed what the move transferred out).
        const char* prelude = R"(
        struct Foo { id: i32; }
        fun delete Foo() { print(f"~Foo {self.id}"); }
        struct Bag { a: uniq Foo; b: uniq Foo; }
        struct Holder { only: uniq Foo; }
        fun take(f: uniq Foo) { }
    )";

        SUBCASE("var decl") {
            std::string src = std::string(prelude) + R"(
            fun main(): i32 {
                var o: Bag = Bag { a = uniq Foo { id = 1 }, b = uniq Foo { id = 2 } };
                var x: uniq Foo = o.a;
                return 0;
            }
        )";
            auto r = VMBackend::run(src.c_str());
            CHECK(r.success);
            CHECK(r.stdout_output == "~Foo 1\n~Foo 2\n");
        }

        SUBCASE("call argument") {
            std::string src = std::string(prelude) + R"(
            fun main(): i32 {
                var o: Bag = Bag { a = uniq Foo { id = 1 }, b = uniq Foo { id = 2 } };
                take(o.a);
                return 0;
            }
        )";
            auto r = VMBackend::run(src.c_str());
            CHECK(r.success);
            CHECK(r.stdout_output == "~Foo 1\n~Foo 2\n");
        }

        SUBCASE("struct literal field") {
            std::string src = std::string(prelude) + R"(
            fun main(): i32 {
                var o: Bag = Bag { a = uniq Foo { id = 1 }, b = uniq Foo { id = 2 } };
                var h: Holder = Holder { only = o.a };
                return 0;
            }
        )";
            auto r = VMBackend::run(src.c_str());
            CHECK(r.success);
            CHECK(r.stdout_output == "~Foo 1\n~Foo 2\n");
        }

        SUBCASE("assign to variable") {
            std::string src = std::string(prelude) + R"(
            fun main(): i32 {
                var o: Bag = Bag { a = uniq Foo { id = 1 }, b = uniq Foo { id = 2 } };
                var y: uniq Foo = nil;
                y = o.a;
                return 0;
            }
        )";
            auto r = VMBackend::run(src.c_str());
            CHECK(r.success);
            CHECK(r.stdout_output == "~Foo 1\n~Foo 2\n");
        }
    }

    TEST_CASE("moving a value-struct field out is rejected") {
        // A noncopyable *value-struct* field can't be moved out (the container
        // can't track a partial move). Must be a compile error.
        const char* source = R"(
        struct Foo { id: i32; }
        fun delete Foo() { print(f"~Foo {self.id}"); }
        struct Inner { val: uniq Foo; }
        struct Outer { a: Inner; b: Inner; }
        fun main(): i32 {
            var o: Outer = Outer {
                a = Inner { val = uniq Foo { id = 1 } },
                b = Inner { val = uniq Foo { id = 2 } }
            };
            var x: Inner = o.a;   // value-struct field move -> error
            return 0;
        }
    )";
        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

}  // TEST_SUITE("E2E RAII")

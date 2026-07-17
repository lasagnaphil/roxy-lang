#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"

using namespace rx;

// ============================================================================
// Heap Allocation Tests - Basic New/Delete
// ============================================================================

TEST_SUITE("E2E Heap") {

    TEST_CASE_TEMPLATE("Heap allocation basic", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 10;
            p.y = 20;
            var result: i32 = p.x + p.y;
            delete p;
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 30);
    }

    TEST_CASE_TEMPLATE("Heap allocation with print", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            p.y = 58;
            print(f"{p.x}");
            print(f"{p.y}");
            delete p;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n58\n");
    }

    TEST_CASE_TEMPLATE("Multiple heap allocations", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p1: uniq Point = uniq Point();
            var p2: uniq Point = uniq Point();
            p1.x = 1;
            p1.y = 2;
            p2.x = 10;
            p2.y = 20;
            print(f"{p1.x}");
            print(f"{p2.x}");
            delete p1;
            delete p2;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n10\n");
    }

    TEST_CASE_TEMPLATE("Heap allocation larger struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Data {
            a: i32;
            b: i32;
            c: i32;
            d: i64;
        }

        fun main(): i32 {
            var d: uniq Data = uniq Data();
            d.a = 1;
            d.b = 2;
            d.c = 3;
            d.d = 100000000000l;
            print(f"{d.a}");
            print(f"{d.b}");
            print(f"{d.c}");
            print(f"{d.d}");
            delete d;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n3\n100000000000\n");
    }

    TEST_CASE_TEMPLATE("Heap allocation with computation", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 5;
            p.y = 7;
            var sum: i32 = p.x + p.y;
            var product: i32 = p.x * p.y;
            print(f"{sum}");
            print(f"{product}");
            delete p;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "12\n35\n");
    }

    TEST_CASE_TEMPLATE("Heap allocation in loop", Backend, RX_E2E_BACKENDS) {
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
                delete c;
            }
            print(f"{sum}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");  // 0 + 10 + 20
    }

    // ============================================================================
    // Heap Allocation Tests - Constraint Reference Model
    // ============================================================================

    TEST_CASE_TEMPLATE("Uniq passed to function (move semantics)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun sum_point(p: uniq Point): i32 {
            return p.x + p.y;
            // p is implicitly deleted at scope exit
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 10;
            p.y = 20;
            var result: i32 = sum_point(p);
            // p is now consumed — no delete needed
            print(f"{result}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    TEST_CASE_TEMPLATE("Heap allocation nested struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        struct Line {
            start: Point;
            end: Point;
        }

        fun main(): i32 {
            var line: uniq Line = uniq Line();
            line.start.x = 0;
            line.start.y = 0;
            line.end.x = 10;
            line.end.y = 10;
            print(f"{line.start.x}");
            print(f"{line.end.x}");
            delete line;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n10\n");
    }

    // ============================================================================
    // Borrow Checking Tests (Constraint Reference Model)
    // ============================================================================

    TEST_CASE_TEMPLATE("Constraint reference borrow check success", Backend, RX_E2E_BACKENDS) {
        // Test that passing uniq to uniq param moves ownership; callee cleans up
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun read_point(p: uniq Point): i32 {
            return p.x + p.y;
            // p is implicitly deleted at scope exit
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            p.y = 8;
            var result: i32 = read_point(p);
            // p is consumed — no delete needed
            print(f"{result}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n");
    }

    TEST_CASE_TEMPLATE("Ref parameter borrow tracking", Backend, RX_E2E_BACKENDS) {
        // Test that ref parameters properly track borrows via RefInc/RefDec
        // The ref parameter increments ref_count at entry, decrements at exit
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun borrow_point(p: ref Point): i32 {
            // p is a borrowed reference - ref_count was incremented at entry
            return p.x + p.y;
            // ref_count is decremented before return
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 10;
            p.y = 25;
            var result: i32 = borrow_point(p);  // Pass uniq as ref
            // After function returns, ref_count is back to 0
            delete p;  // Should succeed - no active borrows
            print(f"{result}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "35\n");
    }

    TEST_CASE_TEMPLATE("Multiple ref borrows in sequence", Backend, RX_E2E_BACKENDS) {
        // Test multiple sequential borrows
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun get_x(p: ref Point): i32 {
            return p.x;
        }

        fun get_y(p: ref Point): i32 {
            return p.y;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 100;
            p.y = 200;
            var x: i32 = get_x(p);  // Borrow 1
            var y: i32 = get_y(p);  // Borrow 2
            delete p;
            print(f"{x}");
            print(f"{y}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n200\n");
    }

    TEST_CASE_TEMPLATE("Ref parameter with multiple returns", Backend, RX_E2E_BACKENDS) {
        // Test that RefDec is emitted on all return paths
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun max_coord(p: ref Point): i32 {
            if (p.x > p.y) {
                return p.x;  // RefDec before this return
            }
            return p.y;  // RefDec before this return too
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 50;
            p.y = 30;
            var m: i32 = max_coord(p);
            delete p;
            print(f"{m}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n");
    }

    TEST_CASE_TEMPLATE("Delete null pointer is safe", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = nil;
            delete p;
            print(f"{42}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    // ============================================================================
    // Borrow Violation Tests
    // ============================================================================

    TEST_CASE("Compile error: delete through ref") {
        // Semantic analysis should reject delete on ref types
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun bad_delete(p: ref Point): i32 {
            delete p;  // ERROR: can't delete through ref
            return 0;
        }

        fun main(): i32 {
            return 0;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("Runtime error: delete with active borrow") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        // Delete should fail at runtime when ref_count > 0
        // Pass same object as both ref (creates borrow) and uniq (for delete)
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun try_delete_while_borrowed(borrowed: ref Point, owner: uniq Point): i32 {
            // borrowed has incremented ref_count to 1
            // Attempting to delete through owner should fail
            delete owner;
            return borrowed.x;  // Would be use-after-free if delete succeeded
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 42;
            var result: i32 = try_delete_while_borrowed(p, p);
            print(f"{result}");
            return 0;
        }
    )";

        auto result = VMBackend::run(source);
        CHECK(result.success == false);  // Should fail - can't delete with active borrow
    }

    // ============================================================================
    // Reference Type Checking Tests
    // ============================================================================

    TEST_CASE("Compile error: ref to uniq assignment") {
        // Cannot assign ref to uniq - would create ownership from borrow
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun bad_assign(p: ref Point): uniq Point {
            var owner: uniq Point = p;  // ERROR: ref -> uniq
            return owner;
        }

        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("Compile error: weak to uniq assignment") {
        // Cannot assign weak to uniq - weak cannot become owner
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun bad_assign(p: weak Point): uniq Point {
            var owner: uniq Point = p;  // ERROR: weak -> uniq
            return owner;
        }

        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("Compile error: weak to ref assignment") {
        // Cannot assign weak to ref - weak cannot become strong borrow
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun bad_assign(p: weak Point): i32 {
            var borrowed: ref Point = p;  // ERROR: weak -> ref
            return borrowed.x;
        }

        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE("Compile error: nil to value type") {
        // Cannot assign nil to non-reference types
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun main(): i32 {
            var p: Point = nil;  // ERROR: nil to value type
            return 0;
        }
    )";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);  // Should fail to compile
    }

    TEST_CASE_TEMPLATE("Valid: uniq to ref conversion", Backend, RX_E2E_BACKENDS) {
        // uniq -> ref is allowed (borrowing from owner)
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun borrow(p: ref Point): i32 {
            return p.x + p.y;
        }

        fun main(): i32 {
            var owner: uniq Point = uniq Point();
            owner.x = 10;
            owner.y = 20;
            var result: i32 = borrow(owner);  // uniq -> ref OK
            delete owner;
            print(f"{result}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "30\n");
    }

    TEST_CASE_TEMPLATE("Valid: nil to uniq assignment", Backend, RX_E2E_BACKENDS) {
        // nil -> uniq is allowed
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun main(): i32 {
            var p: uniq Point = nil;
            delete p;  // Safe to delete nil
            print(f"{42}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    // ============================================================================
    // Weak Reference Tests
    // ============================================================================

    TEST_CASE_TEMPLATE("Weak field assignment and read", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Node {
            value: i32;
        }

        struct Observer {
            target: weak Node;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 42;
            var obs: uniq Observer = uniq Observer();
            obs.target = n;
            // Read through weak ref field
            var w: weak Node = obs.target;
            print(f"{n.value}");
            delete obs;
            delete n;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("Weak local variable from uniq", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var p: uniq Point = uniq Point();
            p.x = 10;
            p.y = 20;
            var w: weak Point = p;
            // Access through the original uniq
            var result: i32 = p.x + p.y;
            delete p;
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 30);
    }

    TEST_CASE_TEMPLATE("Weak nil assignment", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point {
            x: i32;
            y: i32;
        }

        fun main(): i32 {
            var w: weak Point = nil;
            print(f"{42}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "42\n");
    }

    TEST_CASE_TEMPLATE("Weak field in struct with other fields", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Node {
            value: i32;
        }

        struct Observer {
            target: weak Node;
            id: i32;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 99;
            var obs: uniq Observer = uniq Observer();
            obs.target = n;
            obs.id = 1;
            print(f"{obs.id}");
            print(f"{n.value}");
            delete obs;
            delete n;
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n99\n");
    }

    TEST_CASE_TEMPLATE("Weak parameter passing", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun read_weak(w: weak Node): i32 {
            return 1;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 42;
            var w: weak Node = n;
            var result: i32 = read_weak(w);
            delete n;
            return result;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    // ============================================================================
    // Weak References in Containers
    //
    // A `weak T` is {pointer, generation} — 4 slots, so two 64-bit registers.
    // Every step of a container round-trip has to carry both halves; dropping
    // the generation leaves a value that WEAK_CHECK reads as dangling and traps
    // on. These cover the three places that got it wrong: the uniq->weak
    // conversion at a method argument (ir_builder_expr), the argument marshalling
    // for CALL_NATIVE (lowering), and the element read-back (INDEX_GET_LIST).
    // ============================================================================

    TEST_CASE_TEMPLATE("Weak element pushed into List as uniq", Backend, RX_E2E_BACKENDS) {
        // `push(n)` passes a uniq to a `weak T` parameter: the implicit
        // uniq->weak conversion has to happen at the argument.
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 42;
            var watchers: List<weak Node> = List<weak Node>();
            watchers.push(n);
            return watchers[0].value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Weak element pushed into List as weak local", Backend, RX_E2E_BACKENDS) {
        // Already a weak at the call site — exercises the marshalling only.
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 7;
            var w: weak Node = n;
            var watchers: List<weak Node> = List<weak Node>();
            watchers.push(w);
            return watchers[0].value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("List<weak T> with several elements", Backend, RX_E2E_BACKENDS) {
        // Each element must land at its own 4-slot stride.
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var owned: List<uniq Node> = List<uniq Node>();
            for (var i: i32 = 0; i < 4; i = i + 1) {
                var n: uniq Node = uniq Node();
                n.value = (i + 1) * 10;
                owned.push(n);
            }

            var watchers: List<weak Node> = List<weak Node>();
            for (var i: i32 = 0; i < owned.len(); i = i + 1) {
                watchers.push(owned[i]);
            }

            var total: i32 = 0;
            for (var i: i32 = 0; i < watchers.len(); i = i + 1) {
                total = total + watchers[i].value;
            }
            return total;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 100);  // 10 + 20 + 30 + 40
    }

    TEST_CASE_TEMPLATE("Weak value in a Map", Backend, RX_E2E_BACKENDS) {
        // Map values take the same {pointer, generation} round-trip as List
        // elements.
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 99;
            var m: Map<string, weak Node> = Map<string, weak Node>();
            m.insert("a", n);
            return m["a"].value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 99);
    }

    TEST_CASE_TEMPLATE("Weak passed to a user method parameter", Backend, RX_E2E_BACKENDS) {
        // The uniq->weak conversion is indexed off the callee's params, which
        // for a method start with `self` — so this is the same bug as the List
        // push, reached through user code rather than a builtin.
        const char* source = R"(
        struct Node {
            value: i32;
        }

        struct Observer {
            target: weak Node;
        }

        fun Observer.watch(n: weak Node) {
            self.target = n;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 55;
            var obs: uniq Observer = uniq Observer();
            obs.watch(n);
            return obs.target.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 55);
    }

    TEST_CASE_TEMPLATE("List<weak T> round-trips through pop", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Node {
            value: i32;
        }

        fun main(): i32 {
            var n: uniq Node = uniq Node();
            n.value = 13;
            var watchers: List<weak Node> = List<weak Node>();
            watchers.push(n);
            var w: weak Node = watchers.pop();
            return w.value;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 13);
    }

}  // TEST_SUITE("E2E Heap")

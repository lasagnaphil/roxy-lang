#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// Regressions for the lifetime / borrow soundness work (docs/internals/lifetimes.md).
// These exercise the constraint-reference model for `ref` *locals* specifically
// (ref *parameter* borrow traps are covered in test_heap.cpp): a `ref` local is
// a counted borrow, every free path traps while a borrow is live, and the count
// is balanced on all control-flow paths so the owner stays deletable.
TEST_SUITE("E2E Lifetimes") {

    // Finding 1: a `ref` *local* borrow is counted, so freeing the owner while
    // the borrow is live traps — even when the free happens on a non-`delete`
    // path (here, a callee deletes the moved-in owner). Before the fix, ref
    // locals emitted no inc/dec at all, so this was a silent use-after-free.
    TEST_CASE("ref local borrow blocks delete of owner (Finding 1)") {
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun consume_and_free(owner: uniq Point): i32 {
            delete owner;     // owner's object still has b's borrow → traps here
            return 0;
        }

        fun main(): i32 {
            var owner: uniq Point = uniq Point();
            owner.x = 42;
            var b: ref Point = owner;                    // borrow → ref_count = 1
            var unused: i32 = consume_and_free(owner);   // moves owner, deletes it
            return b.x;                                  // would be use-after-free
        }
    )";

        // It compiles — the borrow is caught at runtime (a trap), not statically.
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) != nullptr);

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == false);  // delete traps: object has an active borrow
    }

    // Finding 2: returning a `ref` that borrows a local owner hands the borrow's
    // count off to the caller, so the local owner's RAII drop at function exit
    // sees the still-live borrow and traps at the drop (rather than silently
    // returning a dangling reference).
    TEST_CASE("returning a ref to a local owner traps at the owner's drop (Finding 2)") {
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun make_dangling(): ref Point {
            var owner: uniq Point = uniq Point();
            owner.x = 99;
            var b: ref Point = owner;   // borrow → ref_count = 1
            return b;                   // hand off; owner's drop below sees count 1
        }

        fun main(): i32 {
            var r: ref Point = make_dangling();
            return r.x;
        }
    )";

        // The escape compiles — the design relies on the runtime count to trap
        // at the owner's drop, not a static "cannot return ref to local" rule.
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) != nullptr);

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == false);  // owner's RAII drop traps on the live borrow
    }

    // Balance: ref locals created and dropped across block scopes, a loop with
    // continue/break, and straight-line code all decrement on their exit paths,
    // so the owner's count returns to zero and it stays deletable. This is the
    // positive control proving no decrement is missed (a missed dec would make
    // the explicit `delete owner` trap, or underflow the count).
    TEST_CASE("ref local counting is balanced across control flow") {
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun main(): i32 {
            var owner: uniq Point = uniq Point();
            owner.x = 7;
            var total: i32 = 0;

            // Borrow in a nested block scope.
            {
                var b1: ref Point = owner;
                total = total + b1.x;
            }

            // Borrow inside a loop body, exercising continue and break paths.
            for (var i: i32 = 0; i < 5; i = i + 1) {
                var b2: ref Point = owner;
                if (i == 2) { continue; }
                if (i == 4) { break; }
                total = total + b2.x;
            }

            // Straight-line borrow in its own scope so it drops before the
            // explicit delete below.
            {
                var b3: ref Point = owner;
                total = total + b3.x;
            }

            // All borrows released — count is back to zero, so this succeeds.
            delete owner;
            return total;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 35);  // 7 (block) + 7*3 (loop i=0,1,3) + 7 (b3)
    }

    // Reassigning a ref local through a chain (linked-list walk) keeps the count
    // balanced: each reassignment releases the old borrow and acquires the new,
    // so after the walk every node is deletable.
    TEST_CASE("ref local reassignment keeps the count balanced") {
        const char* source = R"(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            var r: ref Node = ref n1;
            var sum: i32 = r.value;
            r = ref r.next;            // release n1 borrow, acquire n2 borrow
            sum = sum + r.value;
            // r drops here, releasing the n2 borrow; n1 (owning n2) is deletable.
            return sum;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 30);
    }
}

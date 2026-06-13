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

    // Finding 3 (docs/internals/lifetimes.md §9): `container[i] = v` for a
    // noncopyable element must destroy the overwritten element (no leak) and
    // consume the moved-in value (no double-free). Verified via destructor
    // side-effects: the overwritten element's destructor fires once at the
    // assignment, and the moved-in element's fires exactly once at teardown.
    TEST_CASE("index-set destroys old element and consumes the new (Finding 3)") {
        const char* source = R"(
        struct Res { id: i32; }
        fun delete Res() { print(f"del {self.id}"); }

        fun main(): i32 {
            var list: List<uniq Res> = List<uniq Res>();
            list.push(uniq Res { id = 1 });
            list.push(uniq Res { id = 2 });
            list[0] = uniq Res { id = 3 };   // destroy old (1); move new (3) in
            print("---");
            return 0;
            // teardown destroys element 0 (id 3) then element 1 (id 2)
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        // "del 1" proves the old element was destroyed (no leak); a single
        // "del 3" at teardown proves the moved-in value wasn't double-freed.
        CHECK(result.stdout_output == "del 1\n---\ndel 3\ndel 2\n");
    }

    // The moved-in value is move-checked: using it after the index-set is a
    // compile error (proves the semantic consume fires for noncopyable elements
    // despite the borrowed `ref T` target type).
    TEST_CASE("index-set of a noncopyable value moves it (use-after-move is rejected)") {
        const char* source = R"(
        struct Res { id: i32; }

        fun main(): i32 {
            var list: List<uniq Res> = List<uniq Res>();
            list.push(uniq Res { id = 1 });
            var r: uniq Res = uniq Res { id = 2 };
            list[0] = r;          // moves r into the list
            return r.id;          // ERROR: use of moved value 'r'
        }
    )";

        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);  // use-after-move rejected
    }

    // A `ref`-returning function hands off exactly one borrow count to the
    // caller, which the caller *adopts* (no extra increment). After the bound
    // borrow drops, the owner's count is back to zero and it stays deletable —
    // i.e. no over-count (which would make the owner spuriously undeletable).
    TEST_CASE("ref-returning call hands off one count (no over-count)") {
        const char* source = R"(
        struct Item { v: i32; }
        struct Box { item: uniq Item; }

        fun Box.borrow_item(): ref Item {
            var b: ref Item = self.item;   // borrow the heap item via a ref local
            return b;                       // hand off the borrow count
        }

        fun main(): i32 {
            var box: uniq Box = uniq Box { item = uniq Item { v = 42 } };
            var val: i32 = 0;
            {
                var r: ref Item = box.borrow_item();   // adopt the handed-off count
                val = r.v;
            }                                          // r drops -> item count 0
            delete box;                                // succeeds: no over-count
            return val;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    // The handed-off count is real: while the bound borrow is still live, the
    // owner cannot be deleted (the borrow keeps it pinned). This is the
    // counterpart to the no-over-count case — proving the count isn't merely
    // dropped (which would be an under-count / use-after-free).
    TEST_CASE("ref-returning call's borrow blocks delete while live") {
        const char* source = R"(
        struct Item { v: i32; }
        struct Box { item: uniq Item; }

        fun Box.borrow_item(): ref Item {
            var b: ref Item = self.item;
            return b;
        }

        fun main(): i32 {
            var box: uniq Box = uniq Box { item = uniq Item { v = 42 } };
            var r: ref Item = box.borrow_item();   // adopt the live borrow
            delete box;                            // traps: item is still borrowed
            return r.v;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == false);
    }

    // Finding 3 for maps: overwriting an existing key's noncopyable value
    // destroys the old value (guarded by a `contains` check so a *new* key
    // destroys nothing) and consumes the moved-in value. A single key keeps the
    // destructor output deterministic (no Robin-Hood bucket-order ambiguity).
    TEST_CASE("map index-set overwrite destroys old value, new key does not (Finding 3)") {
        const char* source = R"(
        struct Res { id: i32; }
        fun delete Res() { print(f"del {self.id}"); }

        fun main(): i32 {
            var m: Map<i32, uniq Res> = Map<i32, uniq Res>();
            m[1] = uniq Res { id = 1 };   // new key -> no destroy
            m[1] = uniq Res { id = 2 };   // overwrite -> destroy old (1)
            m[1] = uniq Res { id = 3 };   // overwrite -> destroy old (2)
            print("---");
            return 0;
            // teardown destroys the live value (id 3)
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        // No "del" before the first overwrite (new key destroys nothing); each
        // overwritten value destroyed once; the live value destroyed once at end.
        CHECK(result.stdout_output == "del 1\ndel 2\n---\ndel 3\n");
    }
}

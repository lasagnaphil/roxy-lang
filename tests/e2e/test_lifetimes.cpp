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

    // === Phase 2: second-class family (out/inout) ===

    // An `out`/`inout` parameter borrows the caller's value; moving a noncopyable
    // one out of the frame (binding it) would transfer and then free the caller's
    // value -> dangling. Rejected at compile time (lifetimes.md §3).
    TEST_CASE("moving a noncopyable inout out of its frame is rejected") {
        const char* source = R"(
        struct Box { v: i32; }
        fun leak(l: inout List<uniq Box>): i32 {
            var stolen: List<uniq Box> = l;   // moves the caller's list out
            return stolen.len();
        }
        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    // Moving an inout into a closure env (which frees it on drop) is the same
    // escape via a different move site — also rejected.
    TEST_CASE("moving a noncopyable inout into a closure is rejected") {
        const char* source = R"(
        struct Box { v: i32; }
        fun capture(l: inout List<uniq Box>): i32 {
            var f = fun[move l](): i32 => l.len();
            return f();
        }
        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    // Legitimate second-class use stays allowed: mutating an inout in place and
    // passing it onward as another inout argument (the downward path).
    TEST_CASE("inout used in place and passed onward downward is allowed") {
        const char* source = R"(
        struct Box { v: i32; }
        fun fill(l: inout List<uniq Box>): i32 {
            l.push(uniq Box { v = 1 });   // mutate in place
            return l.len();
        }
        fun forward(l: inout List<uniq Box>): i32 {
            return fill(inout l);          // pass onward as inout
        }
        fun main(): i32 {
            var xs: List<uniq Box> = List<uniq Box>();
            return forward(inout xs);
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 1);
    }

    // === Phase 2: [ref self] closure-capture counting ===

    // A closure capturing `self` by reference holds a counted borrow of the
    // receiver, so deleting the receiver while the closure is live traps (the
    // captured ref would otherwise dangle). The copyable receiver + uniq
    // allocation also exercises the promotion gate (the heap check passes, then
    // the count is taken).
    TEST_CASE("ref-self closure capture pins the receiver (delete-while-captured traps)") {
        const char* source = R"(
        struct V { x: i32 = 0; }
        fun V.make_getter(): fun() -> i32 {
            return fun(): i32 => self.x;   // implicit ref-self capture
        }
        fun main(): i32 {
            var u: uniq V = uniq V { x = 5 };
            var g = u.make_getter();   // g's env holds a counted ref to u
            delete u;                  // traps: u is still borrowed by g
            return g();
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == false);
    }

    // The borrow is released when the closure drops, so the receiver becomes
    // deletable again — no over-count.
    TEST_CASE("ref-self capture releases the borrow when the closure drops") {
        const char* source = R"(
        struct V { x: i32 = 0; }
        fun V.make_getter(): fun() -> i32 {
            return fun(): i32 => self.x;
        }
        fun main(): i32 {
            var u: uniq V = uniq V { x = 5 };
            var val: i32 = 0;
            {
                var g = u.make_getter();
                val = g();
            }              // g drops -> env RefDecs u -> count 0
            delete u;      // succeeds
            return val;
        }
    )";

        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 5);
    }

    // ── Call-site receiver counting (lifetimes.md §4/§6) ──
    // A method call on a `uniq` (heap) receiver counts the receiver for the
    // call's duration, so a reentrant free of it traps; the count is balanced on
    // both the normal and the exception-unwind path, leaving the owner deletable.

    // Balance: calling methods on a uniq receiver does not leak or over-count —
    // the object is still destroyed exactly once at scope exit.
    TEST_CASE("method calls on a uniq receiver stay balanced") {
        const char* source = R"(
        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() { print("del"); }
        fun Counter.get(): i32 { return self.value; }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(7);
            var a: i32 = c.get();   // borrow inc/dec around the call, balanced
            var b: i32 = c.get();   // receiver still valid
            return a + b;           // 14
        }
    )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.value == 14);
        CHECK(result.stdout_output == "del\n");  // destroyed exactly once
    }

    // Exception path: a method on a uniq receiver throws. The unwind must
    // release the call-site borrow BEFORE the receiver's own RAII drop, so the
    // owner survives to the in-function catch and is destroyed exactly once.
    // This is the case the naive attempt (sharing the receiver's SSA value)
    // double-deleted — the pinned-copy borrow + deferred record ordering fix it.
    TEST_CASE("uniq receiver survives a throwing method, destroyed once") {
        const char* source = R"(
        struct Boom { msg: string; }
        fun Boom.message(): string for Exception { return self.msg; }

        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() { print("del"); }
        fun Counter.risky(): i32 { throw Boom { msg = "boom" }; }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(5);
            try {
                var unused: i32 = c.risky();  // borrow inc; throws; unwind RefDec
            } catch (e: Boom) {
                print("caught");
            }
            print(f"{c.value}");              // c still alive -> 5
            return 0;
            // c destroyed exactly once at scope exit
        }
    )";
        TestResult result = run_and_capture(source, "main");
        CHECK(result.success == true);
        CHECK(result.stdout_output == "caught\n5\ndel\n");
    }

    // On the *trap-firing* side of this feature: a method call on a `uniq`
    // receiver makes the receiver freeable-by-no-one for the call's duration, so
    // a reentrant free of it traps in object_free (ref_count != 0). That trap
    // mechanism is already covered end-to-end by Findings 1 and 2 above (a live
    // `ref` borrow blocks a delete); the two tests above confirm method calls
    // create and release exactly such a borrow on both the normal and the
    // exception path. A dedicated "free the receiver from inside its own method"
    // test is intentionally omitted: it isn't cleanly constructible in safe Roxy
    // today — a uniq *local* receiver can't be reached from inside the call, a
    // module-global owner doesn't reliably initialize, and inout-`uniq`
    // reassignment (`slot = nil` / `slot = uniq T(..)`) does not yet free the
    // overwritten value (a separate pre-existing gap). So the reach-and-free
    // path can't be expressed without depending on those unrelated holes.
}

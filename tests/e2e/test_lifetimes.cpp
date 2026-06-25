#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

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
    TEST_CASE("ref local borrow blocks delete of owner (Finding 1)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
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

        auto result = VMBackend::run(source);
        CHECK(result.success == false);  // delete traps: object has an active borrow
    }

    // Finding 2: returning a `ref` that borrows a local owner hands the borrow's
    // count off to the caller, so the local owner's RAII drop at function exit
    // sees the still-live borrow and traps at the drop (rather than silently
    // returning a dangling reference).
    TEST_CASE("returning a ref to a local owner traps at the owner's drop (Finding 2)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
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

        auto result = VMBackend::run(source);
        CHECK(result.success == false);  // owner's RAII drop traps on the live borrow
    }

    // Balance: ref locals created and dropped across block scopes, a loop with
    // continue/break, and straight-line code all decrement on their exit paths,
    // so the owner's count returns to zero and it stays deletable. This is the
    // positive control proving no decrement is missed (a missed dec would make
    // the explicit `delete owner` trap, or underflow the count).
    TEST_CASE("ref local counting is balanced across control flow") {  // VM-only: C backend: ref-local RefInc/RefDec count balancing gap
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

        auto result = VMBackend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 35);  // 7 (block) + 7*3 (loop i=0,1,3) + 7 (b3)
    }

    // Reassigning a ref local through a chain (linked-list walk) keeps the count
    // balanced: each reassignment releases the old borrow and acquires the new,
    // so after the walk every node is deletable.
    TEST_CASE_TEMPLATE("ref local reassignment keeps the count balanced", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 30);
    }

    // Finding 3 (docs/internals/lifetimes.md §9): `container[i] = v` for a
    // noncopyable element must destroy the overwritten element (no leak) and
    // consume the moved-in value (no double-free). Verified via destructor
    // side-effects: the overwritten element's destructor fires once at the
    // assignment, and the moved-in element's fires exactly once at teardown.
    TEST_CASE_TEMPLATE("index-set destroys old element and consumes the new (Finding 3)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
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
    TEST_CASE_TEMPLATE("ref-returning call hands off one count (no over-count)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    // The handed-off count is real: while the bound borrow is still live, the
    // owner cannot be deleted (the borrow keeps it pinned). This is the
    // counterpart to the no-over-count case — proving the count isn't merely
    // dropped (which would be an under-count / use-after-free).
    TEST_CASE("ref-returning call's borrow blocks delete while live") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
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

        auto result = VMBackend::run(source);
        CHECK(result.success == false);
    }

    // Finding 3 for maps: overwriting an existing key's noncopyable value
    // destroys the old value (guarded by a `contains` check so a *new* key
    // destroys nothing) and consumes the moved-in value. A single key keeps the
    // destructor output deterministic (no Robin-Hood bucket-order ambiguity).
    TEST_CASE_TEMPLATE("map index-set overwrite destroys old value, new key does not (Finding 3)", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
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
    TEST_CASE_TEMPLATE("inout used in place and passed onward downward is allowed", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 1);
    }

    // === Phase 2: [ref self] closure-capture counting ===

    // A closure capturing `self` by reference holds a counted borrow of the
    // receiver, so deleting the receiver while the closure is live traps (the
    // captured ref would otherwise dangle). The copyable receiver + uniq
    // allocation also exercises the promotion gate (the heap check passes, then
    // the count is taken).
    TEST_CASE("ref-self closure capture pins the receiver (delete-while-captured traps)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
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

        auto result = VMBackend::run(source);
        CHECK(result.success == false);
    }

    // The borrow is released when the closure drops, so the receiver becomes
    // deletable again — no over-count.
    TEST_CASE_TEMPLATE("ref-self capture releases the borrow when the closure drops", Backend, RX_E2E_BACKENDS) {
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

        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 5);
    }

    // ── Call-site receiver counting (lifetimes.md §4/§6) ──
    // A method call on a `uniq` (heap) receiver counts the receiver for the
    // call's duration, so a reentrant free of it traps; the count is balanced on
    // both the normal and the exception-unwind path, leaving the owner deletable.

    // Balance: calling methods on a uniq receiver does not leak or over-count —
    // the object is still destroyed exactly once at scope exit.
    TEST_CASE_TEMPLATE("method calls on a uniq receiver stay balanced", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 14);
        CHECK(result.stdout_output == "del\n");  // destroyed exactly once
    }

    // Exception path: a method on a uniq receiver throws. The unwind must
    // release the call-site borrow BEFORE the receiver's own RAII drop, so the
    // owner survives to the in-function catch and is destroyed exactly once.
    // This is the case the naive attempt (sharing the receiver's SSA value)
    // double-deleted — the pinned-copy borrow + deferred record ordering fix it.
    TEST_CASE_TEMPLATE("uniq receiver survives a throwing method, destroyed once", Backend, RX_E2E_BACKENDS) {
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
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.stdout_output == "caught\n5\ndel\n");
    }

    // Mid-call receiver kill: the receiver `c` is also passed `inout` to the
    // same method, which reassigns it (`slot = nil`) — freeing the object `self`
    // points at. The call-site borrow holds a count on that object, so the free
    // traps instead of leaving `self` dangling. (This is constructible only
    // because inout-`uniq` reassignment now frees the overwritten value; see the
    // dedicated reassignment tests below.) The control omits the reassignment,
    // isolating it as the sole cause of the trap.
    TEST_CASE("freeing a uniq receiver mid-method traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() {}

        fun Counter.kill(slot: inout uniq Counter): i32 {
            slot = nil;     // frees the old object (= the receiver) mid-call
            return 0;
        }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(5);
            return c.kill(inout c);
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);  // compiles → false is a runtime trap
        auto trap = VMBackend::run(trap_src);
        CHECK(trap.success == false);  // delete traps: receiver has an active borrow

        // Control: identical except `kill` does not reassign — succeeds, so the
        // trap above comes from the mid-call free, not the call machinery.
        const char* control_src = R"(
        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() {}

        fun Counter.kill(slot: inout uniq Counter): i32 {
            return 0;
        }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(5);
            return c.kill(inout c);
        }
    )";
        auto control = VMBackend::run(control_src);
        CHECK(control.success == true);
    }

    // ── inout-`uniq` reassignment frees the overwritten value ──
    // Reassigning an owning out/inout pointer must destroy the value it currently
    // points at, or the overwritten object leaks (and the new RHS temp must be
    // consumed, or it's double-owned). This mirrors `uniq`-field assignment.

    // `slot = uniq T(..)`: the old object is freed, the new one stored, and the
    // caller's variable owns exactly the new object (destroyed once at exit).
    TEST_CASE_TEMPLATE("inout uniq reassignment frees the old value, no double-free", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() { print("del"); }

        fun replace(slot: inout uniq Counter) {
            slot = uniq Counter(99);   // frees the old Counter(5), stores Counter(99)
        }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(5);
            replace(inout c);
            return c.value;            // 99 — caller sees the new object
            // c (Counter 99) destroyed once at scope exit
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 99);
        CHECK(result.stdout_output == "del\ndel\n");  // old freed on replace, new freed at exit
    }

    // `slot = nil`: the old object is freed and the slot nulled, so the caller's
    // scope-exit delete is a no-op (no leak, no double-free).
    TEST_CASE_TEMPLATE("inout uniq reassignment to nil frees the old value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Counter { value: i32; }
        fun new Counter(v: i32) { self.value = v; }
        fun delete Counter() { print("del"); }

        fun clear(slot: inout uniq Counter) {
            slot = nil;     // frees the old Counter(5), stores null
        }

        fun main(): i32 {
            var c: uniq Counter = uniq Counter(5);
            clear(inout c);
            return 0;
            // c is nil now; scope-exit delete is a no-op
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.stdout_output == "del\n");  // freed exactly once, by clear()
    }

    // ── Call-site heap-root counting: non-identifier method receivers ──
    // (lifetimes.md §4/§8, §13 "the rest of call-site heap-root counting").
    // The receiver borrow now fires for any statically-heap (`uniq`) receiver,
    // not just a bare identifier: a field-rooted receiver (`a.b.method()`) and a
    // heap-returning temp receiver (`make().method()`).

    // Field-rooted receiver: `o.inner.get()` borrows the heap Inner object for
    // the call. The count is balanced (RefInc before, RefDec after), so the
    // owner and its field destroy exactly once.
    TEST_CASE_TEMPLATE("method call on a field-rooted uniq receiver stays balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Inner { value: i32; }
        fun new Inner(v: i32) { self.value = v; }
        fun delete Inner() { print("del inner"); }
        fun Inner.get(): i32 { return self.value; }

        struct Outer { inner: uniq Inner; }
        fun new Outer() { self.inner = uniq Inner(7); }

        fun main(): i32 {
            var o: uniq Outer = uniq Outer();
            var a: i32 = o.inner.get();   // borrow inc/dec around the call
            var b: i32 = o.inner.get();   // receiver still valid
            return a + b;                 // 14
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 14);
        CHECK(result.stdout_output == "del inner\n");  // destroyed exactly once
    }

    // Heap-temp receiver: `make_inner().get()` borrows the freshly-returned heap
    // temp for the call. The borrow rides a pinned copy, distinct from the temp's
    // own scope-exit Delete, so the temp is destroyed exactly once (no double-free,
    // no spurious trap from a leftover count).
    TEST_CASE_TEMPLATE("method call on a heap-temp uniq receiver stays balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Inner { value: i32; }
        fun new Inner(v: i32) { self.value = v; }
        fun delete Inner() { print("del"); }
        fun Inner.get(): i32 { return self.value; }

        fun make_inner(): uniq Inner { return uniq Inner(9); }

        fun main(): i32 {
            var a: i32 = make_inner().get();   // borrow inc/dec; temp freed after
            return a;                          // 9
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 9);
        CHECK(result.stdout_output == "del\n");  // temp destroyed exactly once
    }

    // Exception path for a heap-temp receiver: a method on a freshly-returned
    // temp throws. The temp carries BOTH its own scope-exit Delete record and the
    // call-site borrow's RefDec; the borrow record is deferred so unwind releases
    // the count BEFORE the temp's Delete, which therefore frees it exactly once
    // (rather than the Delete seeing a leftover borrow and trapping). The temp is
    // scoped to the try body, so unwinding destroys it ("del") before the catch
    // handler runs ("caught"). This is the heap-temp analogue of "uniq receiver
    // survives a throwing method".
    TEST_CASE_TEMPLATE("heap-temp receiver throwing a method is destroyed once", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Boom { msg: string; }
        fun Boom.message(): string for Exception { return self.msg; }

        struct Thing { value: i32; }
        fun new Thing(v: i32) { self.value = v; }
        fun delete Thing() { print("del"); }
        fun Thing.risky(): i32 { throw Boom { msg = "boom" }; }

        fun make_thing(): uniq Thing { return uniq Thing(1); }

        fun main(): i32 {
            try {
                var u: i32 = make_thing().risky();  // temp made, borrowed, throws
            } catch (e: Boom) {
                print("caught");
            }
            return 0;
            // temp destroyed exactly once on unwind
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.stdout_output == "del\ncaught\n");  // temp freed during unwind, then handler
    }

    // Mid-call free of a field-rooted receiver: `o.inner` is both the receiver and
    // is reassigned (`slot = nil`) via an `inout` alias inside the method, freeing
    // the object `self` points at. The call-site borrow on that heap object makes
    // the free trap instead of leaving `self` dangling — the field-rooted analogue
    // of "freeing a uniq receiver mid-method traps".
    TEST_CASE("freeing a field-rooted uniq receiver mid-method traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct Inner { value: i32; }
        fun new Inner(v: i32) { self.value = v; }
        fun delete Inner() {}
        fun Inner.kill(slot: inout uniq Inner): i32 {
            slot = nil;     // frees the old object (= the receiver) mid-call
            return 0;
        }

        struct Outer { inner: uniq Inner; }
        fun new Outer() { self.inner = uniq Inner(5); }
        fun delete Outer() {}

        fun main(): i32 {
            var o: uniq Outer = uniq Outer();
            return o.inner.kill(inout o.inner);
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);  // compiles → false is a runtime trap
        auto trap = VMBackend::run(trap_src);
        CHECK(trap.success == false);  // free traps: receiver has an active borrow

        // Control: identical except `kill` does not reassign — succeeds, isolating
        // the mid-call free as the sole cause of the trap above.
        const char* control_src = R"(
        struct Inner { value: i32; }
        fun new Inner(v: i32) { self.value = v; }
        fun delete Inner() {}
        fun Inner.kill(slot: inout uniq Inner): i32 {
            return 0;
        }

        struct Outer { inner: uniq Inner; }
        fun new Outer() { self.inner = uniq Inner(5); }
        fun delete Outer() {}

        fun main(): i32 {
            var o: uniq Outer = uniq Outer();
            return o.inner.kill(inout o.inner);
        }
    )";
        auto control = VMBackend::run(control_src);
        CHECK(control.success == true);
    }

    // ── Call-site heap-root counting: out/inout arguments ──
    // (lifetimes.md §4/§8, §13 "the rest of call-site heap-root counting").
    // An out/inout argument that points *into* a heap object's storage
    // (`f(inout heap_obj.field)`) counts that heap root for the call, so a
    // mid-call free of the root — through an alias the callee reaches — traps
    // instead of leaving the out/inout pointer dangling.

    // Functional/balance: incrementing through an `inout` into a heap object's
    // field works, and the count is balanced so the owner deletes once afterward.
    TEST_CASE_TEMPLATE("inout into a heap-object field is counted but stays balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Box { value: i32; }
        fun new Box(v: i32) { self.value = v; }
        fun delete Box() { print("del"); }

        fun bump(slot: inout i32) {
            slot = slot + 1;
        }

        fun main(): i32 {
            var b: uniq Box = uniq Box(41);
            bump(inout b.value);     // root `b` counted for the call, then released
            return b.value;          // 42
            // b deleted exactly once at scope exit
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
        CHECK(result.stdout_output == "del\n");  // owner destroyed exactly once
    }

    // Mid-call free of an out/inout heap root: arg 1 (`inout b.value`) points into
    // the heap `Box`, and arg 2 (`inout b`) lets the callee free that same Box
    // mid-call (`owner = nil`). The call-site borrow on the root makes the free
    // trap instead of leaving arg 1 dangling (the subsequent `slot = ...` would
    // otherwise write into freed storage).
    TEST_CASE("freeing an out/inout heap root mid-call traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct Box { value: i32; }
        fun new Box(v: i32) { self.value = v; }
        fun delete Box() {}

        fun evil(slot: inout i32, owner: inout uniq Box): i32 {
            owner = nil;     // frees the Box that `slot` points into → mid-call free
            slot = 5;        // would write into freed storage
            return 0;
        }

        fun main(): i32 {
            var b: uniq Box = uniq Box(1);
            return evil(inout b.value, inout b);   // root of arg 1 is `b`, killed via arg 2
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);  // compiles → false is a runtime trap
        auto trap = VMBackend::run(trap_src);
        CHECK(trap.success == false);  // free traps: the heap root has an active borrow

        // Control: identical except `evil` does not free the root — succeeds,
        // isolating the mid-call free as the sole cause of the trap above.
        const char* control_src = R"(
        struct Box { value: i32; }
        fun new Box(v: i32) { self.value = v; }
        fun delete Box() {}

        fun evil(slot: inout i32, owner: inout uniq Box): i32 {
            slot = 5;
            return 0;
        }

        fun main(): i32 {
            var b: uniq Box = uniq Box(1);
            return evil(inout b.value, inout b);
        }
    )";
        auto control = VMBackend::run(control_src);
        CHECK(control.success == true);
    }

    // ── Non-capture `self` promotions ──
    // (lifetimes.md §6, §13). Binding / returning / storing `self` as a
    // first-class `ref` promotes the second-class receiver borrow to a counted
    // one — sound only on a heap receiver. Since a method can be called on a
    // stack or heap receiver, the promotion's RefInc is guarded by a runtime
    // AssertHeap that traps a stack receiver before the (header-writing) inc.

    // Bind, heap receiver: `var r: ref P = self` on a `uniq` receiver passes the
    // heap gate, counts the borrow, and stays balanced (receiver destroyed once).
    TEST_CASE_TEMPLATE("binding self to a ref on a uniq receiver is counted and balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }
        fun P.bind_ref(): i32 {
            var r: ref P = self;   // promotion: heap gate passes, borrow counted
            return r.x;            // r's RefDec at method exit keeps it balanced
        }

        fun main(): i32 {
            var p: uniq P = uniq P(42);
            return p.bind_ref();   // 42
            // p destroyed exactly once at scope exit
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
        CHECK(result.stdout_output == "del\n");
    }

    // Bind, stack receiver: the same method on a stack value receiver traps at the
    // promotion's heap gate (the would-be borrow inc would write into a bogus,
    // stack-relative object header). Covered for both a copyable receiver and a
    // NONCOPYABLE one — the latter is the case the capture path's copyable-only
    // gate would miss, so the always-on promotion gate is strictly sounder.
    TEST_CASE("binding self to a ref on a stack receiver traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* copyable_src = R"(
        struct P { x: i32; }
        fun P.bind_ref(): i32 {
            var r: ref P = self;
            return r.x;
        }
        fun main(): i32 {
            var p: P = P { x = 5 };   // stack (copyable) receiver
            return p.bind_ref();      // traps: self is stack-allocated
        }
    )";
        BumpAllocator a1(65536);
        CHECK(compile(a1, copyable_src) != nullptr);
        CHECK(VMBackend::run(copyable_src).success == false);

        const char* noncopyable_src = R"(
        struct Q { x: i32; }
        fun new Q(v: i32) { self.x = v; }
        fun delete Q() {}
        fun Q.bind_ref(): i32 {
            var r: ref Q = self;
            return r.x;
        }
        fun main(): i32 {
            var q: Q = Q(5);          // stack NONCOPYABLE value-struct receiver
            return q.bind_ref();      // traps: self is stack-allocated
        }
    )";
        BumpAllocator a2(65536);
        CHECK(compile(a2, noncopyable_src) != nullptr);
        CHECK(VMBackend::run(noncopyable_src).success == false);
    }

    // Return, heap receiver: `return self` from a `ref`-returning method hands off
    // the borrow count to the caller (passes the heap gate first).
    TEST_CASE_TEMPLATE("returning self as a ref hands off the borrow (uniq receiver)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }
        fun P.as_ref(): ref P { return self; }

        fun main(): i32 {
            var p: uniq P = uniq P(42);
            var r: ref P = p.as_ref();   // adopts the handed-off count
            return r.x;                  // 42
            // r's RefDec then p's delete at scope exit — balanced
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
        CHECK(result.stdout_output == "del\n");
    }

    // Return, stack receiver: `return self` traps at the heap gate.
    TEST_CASE("returning self as a ref from a stack receiver traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* source = R"(
        struct P { x: i32; }
        fun P.as_ref(): ref P { return self; }
        fun main(): i32 {
            var p: P = P { x = 5 };       // stack receiver
            var r: ref P = p.as_ref();    // traps: self is stack-allocated
            return r.x;
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) != nullptr);
        CHECK(VMBackend::run(source).success == false);
    }

    // Store, stack receiver: `r = self` (reassigning a ref local) is also a
    // promotion. Bound first to a heap backup so only the `r = self` store can
    // trap, isolating the store path's gate.
    TEST_CASE("storing self into a ref local traps on a stack receiver") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}
        fun P.rebind(backup: ref P): i32 {
            var r: ref P = backup;   // bind to a heap source (no self gate)
            r = self;                // STORE promotion: traps if self is stack
            return r.x;
        }

        fun main(): i32 {
            var heap: uniq P = uniq P(1);
            var p: P = P(5);             // stack receiver
            return p.rebind(heap);       // traps at `r = self`
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) != nullptr);
        CHECK(VMBackend::run(source).success == false);
    }

    // Pass to a ref param, heap receiver: `take_ref(self)` lets the callee count
    // `self` (its ref param's entry inc). On a `uniq` receiver the call-site heap
    // gate passes and the count is balanced (callee dec on exit).
    TEST_CASE_TEMPLATE("passing self to a ref param on a uniq receiver is balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }
        fun take_ref(r: ref P): i32 { return r.x; }
        fun P.pass(): i32 { return take_ref(self); }

        fun main(): i32 {
            var p: uniq P = uniq P(7);
            return p.pass();   // 7; heap gate passes, borrow balanced
            // p destroyed exactly once at scope exit
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
        CHECK(result.stdout_output == "del\n");
    }

    // Pass to a ref param, stack receiver: the call-site heap gate traps before the
    // callee's entry inc would corrupt a bogus header. Covers a free-function
    // callee (param offset 0) and a method callee (offset 1, past the receiver).
    TEST_CASE("passing self to a ref param on a stack receiver traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* free_src = R"(
        struct P { x: i32; }
        fun take_ref(r: ref P): i32 { return r.x; }
        fun P.pass(): i32 { return take_ref(self); }
        fun main(): i32 {
            var p: P = P { x = 5 };   // stack receiver
            return p.pass();          // take_ref(self): self stack → traps
        }
    )";
        BumpAllocator a1(65536);
        CHECK(compile(a1, free_src) != nullptr);
        CHECK(VMBackend::run(free_src).success == false);

        const char* method_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}
        fun P.helper(r: ref P): i32 { return r.x; }
        fun P.pass(other: ref P): i32 { return other.helper(self); }
        fun main(): i32 {
            var o: uniq P = uniq P(1);
            var p: P = P(5);          // stack receiver
            return p.pass(o);         // other.helper(self): self stack → traps (offset 1)
        }
    )";
        BumpAllocator a2(65536);
        CHECK(compile(a2, method_src) != nullptr);
        CHECK(VMBackend::run(method_src).success == false);
    }

    // ── Container element lvalues: `inout`/`out list[i]` (lifetimes.md §15) ──
    // Phase 2: the `INDEX_ADDR` op makes `list[i]` a true lvalue, so a callee
    // mutates the element in place through its buffer address. These exercise the
    // *functional* single-argument case — the callee only receives the element
    // pointer, so it can't realloc/free the container, making it sound without the
    // pin (the pin + adversarial traps land in Phase 3). Both backends.

    TEST_CASE_TEMPLATE("inout of a List<i32> element mutates it in place", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bump(slot: inout i32) { slot = slot + 1; }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(10);
            xs.push(20);
            bump(inout xs[0]);
            bump(inout xs[1]);
            bump(inout xs[1]);
            return xs[0] + xs[1];   // 11 + 22 = 33
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 33);
    }

    TEST_CASE_TEMPLATE("out of a List<i32> element writes through the address", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun set42(slot: out i32) { slot = 42; }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(0);
            set42(out xs[0]);
            return xs[0];   // 42
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("inout of a wide (2-slot) List<i64> element", Backend, RX_E2E_BACKENDS) {
        // The value needs all 8 bytes; the result is reduced to a small code so it
        // survives the C backend's 8-bit exit-code capture while still proving the
        // full i64 round-tripped through the element address.
        const char* source = R"(
        fun add(slot: inout i64, d: i64) { slot = slot + d; }

        fun main(): i64 {
            var xs: List<i64> = List<i64>();
            xs.push(1000000000000l);
            add(inout xs[0], 1l);
            if (xs[0] == 1000000000001l) { return 7l; }
            return 0l;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
    }

    TEST_CASE_TEMPLATE("inout of a struct List element mutates it in place", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Vec2 { x: i32; y: i32; }
        fun move_right(v: inout Vec2, d: i32) { v.x = v.x + d; }

        fun main(): i32 {
            var pts: List<Vec2> = List<Vec2>();
            pts.push(Vec2 { x = 1, y = 2 });
            move_right(inout pts[0], 10);
            return pts[0].x + pts[0].y;   // 11 + 2 = 13
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 13);
    }

    TEST_CASE_TEMPLATE("inout of a Map value mutates it in place", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bump(slot: inout i64) { slot = slot + 1; }

        fun main(): i64 {
            var m: Map<i64, i64> = Map<i64, i64>();
            m.insert(7l, 100l);
            bump(inout m[7l]);
            bump(inout m[7l]);
            return m[7l];   // 102
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 102);
    }

    // ── Phase 3: the container element-borrow pin (adversarial cases) ──
    // The call site pins the container while an `inout`/`out` element is borrowed,
    // so a mid-call realloc (push) or free (reassign) of that same container —
    // reached via a second argument — traps instead of dangling the element
    // pointer. (VM-only, like the other runtime-trap tests; the C backend refuses
    // the mutation too — memory-safe — but its clean trap reporting is deferred.)

    TEST_CASE("mid-call push of a borrowed List traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        fun evil(slot: inout i32, lst: inout List<i32>): i32 {
            lst.push(99);   // reallocs the buffer that `slot` points into → traps
            slot = 5;
            return 0;
        }
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(10);
            return evil(inout xs[0], inout xs);
        }
    )";
        BumpAllocator a1(65536);
        CHECK(compile(a1, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);

        // Control: same shape without the mutation — succeeds.
        const char* control_src = R"(
        fun ok(slot: inout i32, lst: inout List<i32>): i32 {
            slot = 5;
            return 0;
        }
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(10);
            return ok(inout xs[0], inout xs);
        }
    )";
        CHECK(VMBackend::run(control_src).success == true);
    }

    // (A mid-call *free* of the container is a non-threat for a copyable container
    // like List<i32>: reassigning it doesn't free the backing buffer, so the
    // element pointer can't dangle that way. The realloc threat above is the live
    // one. The delete-guards in roxy_rt / delete_value are correct defensive code
    // for when owning-element containers — `List<uniq T>` element `inout`, whose
    // delete does free the buffer — are supported; that's the deferred next step.)

    TEST_CASE_TEMPLATE("nested element borrows of one container stay balanced", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun add_both(a: inout i32, b: inout i32) { a = a + 10; b = b + 20; }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            xs.push(2);
            add_both(inout xs[0], inout xs[1]);   // two pins on xs (count 2)
            xs.push(3);                           // count back to 0 → push works again
            return xs[0] + xs[1] + xs[2];         // 11 + 22 + 3 = 36 (fits the C exit code)
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 36);
    }

    TEST_CASE_TEMPLATE("in-place set of a borrowed container is allowed", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun edit(slot: inout i32, lst: inout List<i32>): i32 {
            lst[1] = 99;   // in-place set (no realloc) — allowed while pinned
            slot = 5;
            return 0;
        }
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(10);
            xs.push(20);
            var r: i32 = edit(inout xs[0], inout xs);
            return xs[0] + xs[1];   // 5 + 99 = 104
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 104);
    }

    // An exception thrown through an `inout list[i]` call must unpin the container
    // on unwind (the deferred Unpin cleanup record), or the container would be left
    // permanently frozen — the later push would then spuriously trap.
    TEST_CASE_TEMPLATE("exception through an index-inout call unpins the container", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Boom { msg: string; }
        fun Boom.message(): string for Exception { return self.msg; }

        fun risky(slot: inout i32): i32 { throw Boom { msg = "x" }; }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            try {
                var u: i32 = risky(inout xs[0]);   // pins xs; throws before unpin
            } catch (e: Boom) {
                xs.push(2);   // xs must be unpinned by the unwind → push works
            }
            return xs[0] + xs[1] + xs.len();   // 1 + 2 + 2 = 5
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 5);
    }

    // ── Owning-element containers: `inout`/`out` of a `uniq` element ──
    // (lifetimes.md §15.) `inout list[i]` on a `List<uniq T>` re-types to the raw
    // element type `uniq T` (not the `borrowed` read view `ref T`), so the callee
    // gets reassignable access to the owning slot. Reassigning frees the old
    // pointee in place; the container still owns the new one, destroyed once at
    // scope exit.

    TEST_CASE_TEMPLATE("inout of a List<uniq T> element reassigns it in place", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }
        fun replace(slot: inout uniq P) { slot = uniq P(99); }

        fun main(): i32 {
            var xs: List<uniq P> = List<uniq P>();
            xs.push(uniq P(1));
            replace(inout xs[0]);   // frees P(1) in place, stores P(99)
            return xs[0].x;         // 99 (xs still owns the new element)
            // xs deleted at scope exit → frees P(99)
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 99);
        CHECK(result.stdout_output == "del\ndel\n");  // old freed on replace, new at exit
    }

    TEST_CASE_TEMPLATE("inout of a Map<K, uniq V> value reassigns it in place", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}
        fun replace(slot: inout uniq P) { slot = uniq P(42); }

        fun main(): i32 {
            var m: Map<i64, uniq P> = Map<i64, uniq P>();
            m.insert(7l, uniq P(1));
            replace(inout m[7l]);
            return m[7l].x;   // 42
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    // For a *noncopyable* container the delete genuinely frees the buffer, so the
    // free-guards (Phase 3) are now live: freeing the borrowed List mid-call traps.
    TEST_CASE("mid-call free of a borrowed List<uniq T> traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}
        fun evil(slot: inout uniq P, lst: inout List<uniq P>): i32 {
            lst = List<uniq P>();   // frees the old (borrowed) list → traps
            return 0;
        }
        fun main(): i32 {
            var xs: List<uniq P> = List<uniq P>();
            xs.push(uniq P(1));
            return evil(inout xs[0], inout xs);
        }
    )";
        BumpAllocator a1(65536);
        CHECK(compile(a1, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);
    }

    TEST_CASE("mid-call push of a borrowed List<uniq T> traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}
        fun evil(slot: inout uniq P, lst: inout List<uniq P>): i32 {
            lst.push(uniq P(2));   // reallocs the borrowed buffer → traps
            return 0;
        }
        fun main(): i32 {
            var xs: List<uniq P> = List<uniq P>();
            xs.push(uniq P(1));
            return evil(inout xs[0], inout xs);
        }
    )";
        BumpAllocator a1(65536);
        CHECK(compile(a1, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);
    }

    // ── List/Map reference counting: `List<ref T>` holds counted borrows ──
    // (lifetimes.md §8.) A container of `ref` is noncopyable (move-only), so it is
    // destroyed; push acquires a count on the pointee, container-destroy releases
    // it. Deleting the owner while the container still borrows it traps.

    TEST_CASE_TEMPLATE("List<ref T> push acquires a count, destroy releases it", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var owner: uniq P = uniq P(7);
            var refs: List<ref P> = List<ref P>();
            refs.push(owner);      // borrow counted (RefInc)
            var v: i32 = owner.x;  // 7
            return v;
            // scope exit (LIFO): refs destroyed first -> RefDec owner; then owner
            // is deletable (count 0) -> destroyed exactly once.
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
        CHECK(result.stdout_output == "del\n");  // owner destroyed exactly once
    }

    TEST_CASE("deleting an owner still borrowed by a List<ref T> traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}

        fun main(): i32 {
            var refs: List<ref P> = List<ref P>();
            var owner: uniq P = uniq P(7);
            refs.push(owner);   // borrow counted
            delete owner;       // still borrowed by refs -> traps
            return 0;
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);

        // Control: no push -> the owner is unborrowed -> delete succeeds.
        const char* control_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}

        fun main(): i32 {
            var refs: List<ref P> = List<ref P>();
            var owner: uniq P = uniq P(7);
            delete owner;
            return 0;
        }
    )";
        CHECK(VMBackend::run(control_src).success == true);
    }

    TEST_CASE_TEMPLATE("List<ref T> pop hands the borrow off to the caller", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var owner: uniq P = uniq P(7);
            var refs: List<ref P> = List<ref P>();
            refs.push(owner);            // count 1 (container holds)
            var r: ref P = refs.pop();   // hand-off: count stays 1, now r's
            var v: i32 = r.x;            // 7
            return v;
            // scope exit (LIFO): r -> RefDec (count 0); refs (empty); owner deleted once
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
        CHECK(result.stdout_output == "del\n");
    }

    TEST_CASE_TEMPLATE("List<ref T> overwrite releases the old borrow, acquires the new", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var a: uniq P = uniq P(1);
            var b: uniq P = uniq P(2);
            var refs: List<ref P> = List<ref P>();
            refs.push(a);            // a count 1
            refs[0] = b;             // RefDec a (0), RefInc b (1)
            var v: i32 = refs[0].x;  // 2
            return v;
            // scope exit: refs -> RefDec b (0); then b, a deletable (count 0) -> two dels
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 2);
        CHECK(result.stdout_output == "del\ndel\n");
    }

    // ── `Map<_, ref V>` holds counted borrows (lifetimes.md §8 / §13) ──
    // Insert acquires a count on the value pointee, remove / clear / destroy
    // release it (runtime flag set by __map_mark_ref_values at construction;
    // destroy RefDec's via the BCDeleteDesc::RefDec value descriptor).

    TEST_CASE_TEMPLATE("Map<_, ref V> insert acquires a count, destroy releases it", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var owner: uniq P = uniq P(7);
            var m: Map<i32, ref P> = Map<i32, ref P>();
            m.insert(1, owner);    // borrow counted (RefInc)
            var v: i32 = owner.x;  // 7
            return v;
            // scope exit (LIFO): m destroyed -> RefDec owner; owner deletable -> one "del"
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
        CHECK(result.stdout_output == "del\n");
    }

    TEST_CASE("deleting an owner still borrowed by a Map<_, ref V> traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() {}

        fun main(): i32 {
            var m: Map<i32, ref P> = Map<i32, ref P>();
            var owner: uniq P = uniq P(7);
            m.insert(1, owner);   // borrow counted
            delete owner;         // still borrowed by m -> traps
            return 0;
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);
    }

    TEST_CASE_TEMPLATE("Map<_, ref V> remove releases the borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var owner: uniq P = uniq P(7);
            var m: Map<i32, ref P> = Map<i32, ref P>();
            m.insert(1, owner);   // count 1
            m.remove(1);          // RefDec -> count 0
            delete owner;         // deletable -> "del"
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.stdout_output == "del\n");
    }

    TEST_CASE_TEMPLATE("Map<_, ref V> clear releases all borrows", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var owner: uniq P = uniq P(7);
            var m: Map<i32, ref P> = Map<i32, ref P>();
            m.insert(1, owner);
            m.insert(2, owner);   // count 2
            m.clear();            // RefDec x2 -> count 0
            delete owner;         // deletable -> "del"
            return 0;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.stdout_output == "del\n");
    }

    TEST_CASE_TEMPLATE("Map<_, ref V> insert-replace releases the old borrow, acquires the new", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        fun new P(v: i32) { self.x = v; }
        fun delete P() { print("del"); }

        fun main(): i32 {
            var a: uniq P = uniq P(1);
            var b: uniq P = uniq P(2);
            var m: Map<i32, ref P> = Map<i32, ref P>();
            m.insert(1, a);       // a count 1
            m.insert(1, b);       // replace: RefDec a (0), RefInc b (1)
            var v: i32 = m[1].x;  // 2
            return v;
            // scope exit: m -> RefDec b (0); then b, a deletable -> two dels
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 2);
        CHECK(result.stdout_output == "del\ndel\n");
    }

    // ── Struct holding a `ref` field (lifecycle-traits.md step 3) ──
    // A struct with a `ref` field is move-only (like List<ref>) and counts the
    // borrow: construction ref_incs, drop ref_decs, overwrite rebalances, and a
    // move transfers the borrow without a count change. So a borrow stored in a
    // struct keeps its owner alive (or traps), exactly like a List<ref> element.

    TEST_CASE_TEMPLATE("ref field: construct counts, read works, drop releases", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            o.x = 9;
            { var h: H = H { r = o }; }   // borrow counted, released at block end
            delete o;                      // count back to 0 → deletable
            return o.x;                    // (o still valid: never freed)
        }
    )";
        // NOTE: read o.x after delete is unsound in general, but here we only assert
        // the success/value to prove the count balanced. Use a pre-delete read:
        const char* safe = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            o.x = 9;
            var v: i32 = 0;
            { var h: H = H { r = o }; v = h.r.x; }
            delete o;
            return v;
        }
    )";
        (void)source;
        auto result = Backend::run(safe);
        CHECK(result.success == true);
        CHECK(result.value == 9);
    }

    TEST_CASE("ref field: deleting the owner while a struct borrows it traps") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* trap_src = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            var h: H = H { r = o };   // borrow counted
            delete o;                  // still borrowed → traps
            return 0;
        }
    )";
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, trap_src) != nullptr);
        CHECK(VMBackend::run(trap_src).success == false);
    }

    TEST_CASE_TEMPLATE("ref field: overwrite releases the old borrow, acquires the new", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun main(): i32 {
            var a: uniq P = uniq P();
            var b: uniq P = uniq P();
            var h: H = H { r = a };   // a borrowed
            h.r = b;                   // release a, acquire b
            delete a;                  // a no longer borrowed → ok
            var v: i32 = h.r.x;        // b still borrowed by h
            return v;
            // teardown: h drops (release b); then b deletable
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
    }

    TEST_CASE("ref field: overwrite leaves the new borrow counted (delete new traps)") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* src = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun main(): i32 {
            var a: uniq P = uniq P();
            var b: uniq P = uniq P();
            var h: H = H { r = a };
            h.r = b;       // b now borrowed by h
            delete b;      // still borrowed → traps
            return 0;
        }
    )";
        CHECK(VMBackend::run(src).success == false);
    }

    TEST_CASE_TEMPLATE("ref field: passing the struct by value moves the borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct P { x: i32; }
        struct H { r: ref P; }
        fun take(h: H): i32 { return h.r.x; }
        fun main(): i32 {
            var o: uniq P = uniq P();
            o.x = 7;
            var h: H = H { r = o };   // borrow counted
            var v: i32 = take(h);      // move h into take; its drop releases the borrow
            delete o;                  // count back to 0 → ok
            return v;
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
    }

    // ── Map<_, uniq V>: remove / clear destroy the values (lifecycle-traits.md
    // step 4) ── These previously leaked: only map-destroy and `m[k]=v` ran value
    // destructors; the remove/clear methods discarded values without destroying
    // them. The cleanup is emitted as ordinary IR (contains-guarded delete /
    // bucket-iteration delete-loop), so both backends get it.

    TEST_CASE_TEMPLATE("Map<_, uniq V>: remove destroys the removed value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct R { id: i32; }
        fun delete R() { print(f"del {self.id}"); }
        fun main(): i32 {
            var m: Map<i32, uniq R> = Map<i32, uniq R>();
            m.insert(1, uniq R { id = 1 });
            m.insert(2, uniq R { id = 2 });
            m.remove(1);          // destroys value 1 here
            print("--");
            return 0;
            // teardown destroys the remaining value (2)
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        // "del 1" before "--" proves remove destroyed it (no leak); "del 2" after
        // proves the survivor is destroyed exactly once at teardown.
        CHECK(result.stdout_output == "del 1\n--\ndel 2\n");
    }

    TEST_CASE_TEMPLATE("Map<_, uniq V>: clear destroys all values", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct R { id: i32; }
        fun delete R() { print("del"); }
        fun main(): i32 {
            var m: Map<i32, uniq R> = Map<i32, uniq R>();
            m.insert(1, uniq R { id = 1 });
            m.insert(2, uniq R { id = 2 });
            m.insert(3, uniq R { id = 3 });
            m.clear();            // destroys all three here
            print("--");
            var n: i32 = m.len();
            return n;             // 0 — map is empty and reusable
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 0);
        // three "del" before "--" (all destroyed by clear), none after.
        CHECK(result.stdout_output == "del\ndel\ndel\n--\n");
    }

    TEST_CASE_TEMPLATE("Map<_, uniq V>: remove leaves the map usable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct R { id: i32; }
        fun delete R() {}
        fun main(): i32 {
            var m: Map<i32, uniq R> = Map<i32, uniq R>();
            m.insert(1, uniq R { id = 10 });
            m.insert(2, uniq R { id = 20 });
            m.remove(1);
            return m[2].id + m.len();   // 20 + 1
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 21);
    }

    // insert-replace: inserting a duplicate key destroys the old value. Its value
    // arg is consumed *after* the insert (deferred past the contains-guard branch),
    // so a fresh struct-literal value survives the insert rather than being nulled
    // early (the prior bug). Both a temporary value and a new-key insert are
    // covered to guard the regression in both directions.
    TEST_CASE_TEMPLATE("Map<_, uniq V>: insert-replace destroys the old value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct R { id: i32; }
        fun delete R() { print(f"del {self.id}"); }
        fun main(): i32 {
            var m: Map<i32, uniq R> = Map<i32, uniq R>();
            m.insert(1, uniq R { id = 1 });
            m.insert(1, uniq R { id = 2 });   // destroys old (1) here; stores new (2)
            print("--");
            return m[1].id;                    // 2 — new value is intact
            // teardown destroys the surviving value (2)
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 2);
        CHECK(result.stdout_output == "del 1\n--\ndel 2\n");
    }

    TEST_CASE_TEMPLATE("Map<_, uniq V>: insert with a new key keeps the value (no early-null)", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct R { id: i32; }
        fun delete R() {}
        fun main(): i32 {
            var m: Map<i32, uniq R> = Map<i32, uniq R>();
            m.insert(1, uniq R { id = 7 });   // new key: nothing destroyed, value stored
            return m[1].id;                    // 7 — the value wasn't nulled by the cleanup path
        }
    )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
    }

}

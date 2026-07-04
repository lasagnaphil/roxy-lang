#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/rt/slab_allocator.hpp"

using namespace rx;

// ─────────────────────────────────────────────────────────────────────────────
// Known-open lifetime / borrow soundness bugs (audit 2026-07-04).
//
// Each case here asserts the CORRECT (sound) behavior, so it FAILS today. To
// keep the suite green while the bugs are open, every case is decorated with
// `doctest::should_fail()`: doctest reports an expected failure as a pass. When
// a bug is fixed, its case starts genuinely passing, and `should_fail` then
// reports it as an UNEXPECTED PASS (a red result) — that is the signal to
// delete the decorator (and, for the memory-model doc, update lifetimes.md).
//
// All VM-only (plain TEST_CASE, no C-backend template): the C backend has its
// own separate documented gaps, and several of these drive runtime traps the C
// backend renders as a raw abort rather than a catchable VM error.
//
// Findings, by severity:
//   1  inout list[i].field  — silent use-after-free write (memory-unsafe)
//   2  tagged-union discriminant reassignment — leaks/frees stale variant bytes
//   3  ref local + exception unwind out of frame — leaked borrow, undeletable owner
//   4  coroutine ref local across a yield — leaked borrow on mid-iteration destroy
//   5  List<uniq T>.copy() — shallow copy of owned pointers → double free
//   6  List<ref T>.copy() / Map<_,ref V>.values() — borrows copied without RefInc
//   7  weak member access emits no WeakCheck — silent dangling read
//   8  global ref/weak is uncounted — does not block delete of its owner
//   9  caught-exception object and string temporaries are never freed (leaks)
// ─────────────────────────────────────────────────────────────────────────────

TEST_SUITE("E2E Lifetime Regressions") {

    // Finding 1 — FIXED. `f(inout list[i].field)` passes the interior address of
    // a field of a container element. Unlike bare `inout list[i]`, this
    // sub-lvalue used to get neither the container pin nor the heap-root count,
    // so a mid-call structural mutation silently dangled the pointer (a
    // reallocating push made the callee's write land in freed memory). The fix
    // extends the call-site pin to any out/inout lvalue that roots in a
    // container subscript, so mutate-while-borrowed now traps here exactly as it
    // does for bare `inout list[i]`. (Mirrors "mid-call push of a borrowed List
    // traps" in test_lifetimes.cpp.)
    TEST_CASE("F1 inout of a container-element field pins the container") {
        // Structural mutation while an element field is borrowed → trap.
        const char* trap_src = R"(
        struct P { x: i32; }
        fun grow_then_write(x: inout i32, l: inout List<P>): i32 {
            l.push(P { x = 0 });   // reallocs the buffer `x` points into → traps
            x = 42;
            return 0;
        }
        fun main(): i32 {
            var l: List<P> = List<P>();
            l.push(P { x = 1 });
            return grow_then_write(inout l[0].x, inout l);
        }
        )";
        CHECK(VMBackend::run(trap_src).success == false);

        // Control: same shape, no structural mutation → the in-place write lands.
        const char* control_src = R"(
        struct P { x: i32; }
        fun write_only(x: inout i32, l: inout List<P>): i32 {
            x = 42;
            return 0;
        }
        fun main(): i32 {
            var l: List<P> = List<P>();
            l.push(P { x = 1 });
            write_only(inout l[0].x, inout l);
            return l[0].x;
        }
        )";
        auto r = VMBackend::run(control_src);
        CHECK(r.success == true);
        CHECK(r.value == 42);
    }

    // Finding 2 — FIXED. Reassigning a tagged-union discriminant used to run no
    // cleanup of the variant it leaves. Because the destructor's variant cleanup
    // is guarded by the *current* discriminant, this leaked the outgoing
    // variant's owned field (leak direction), and switching *to* an owning
    // variant made teardown free the leftover union bytes as a pointer (crash
    // direction). The fix drops the outgoing variant's owned fields on the
    // discriminant write and zeroes the union so the incoming variant reads null.
    TEST_CASE("F2 switching a tagged-union discriminant cleans up the old variant") {
        // Leak direction: switching A→B frees A's uniq (its destructor runs).
        const char* leak_src = R"(
        enum K { A, B }
        struct Owner { val: i32; }
        fun delete Owner() { print("Owner dtor"); }
        struct S { when kind: K { case A: o: uniq Owner; case B: n: i32; } }
        fun main(): i32 {
            var s: S = S { kind = K::A, o = uniq Owner { val = 9 } };
            s.kind = K::B;   // leaves variant A → its uniq Owner is freed here
            return 0;
        }
        )";
        auto leak = VMBackend::run(leak_src);
        CHECK(leak.success == true);
        CHECK(leak.stdout_output.find("Owner dtor") != String::npos);

        // Crash direction: build the non-owning variant, switch to the owning one
        // without setting its field → teardown must NOT free stale union bytes.
        const char* crash_src = R"(
        enum K { A, B }
        struct Owner { val: i32; }
        fun delete Owner() { print("Owner dtor"); }
        struct S { when kind: K { case A: o: uniq Owner; case B: n: i32; } }
        fun main(): i32 {
            var s: S = S { kind = K::B, n = 12345 };
            s.kind = K::A;   // union holds stale 12345; A.o must read null, not free it
            return 0;
        }
        )";
        auto crash = VMBackend::run(crash_src);
        CHECK(crash.success == true);
        // The (unset) A.o must have been zeroed, so no spurious destructor ran.
        CHECK(crash.stdout_output.find("Owner dtor") == String::npos);
    }

    // Finding 3 — FIXED. A `ref` local that borrows a `ref` parameter shares its
    // register; when an exception unwound OUT of the frame, the VM unwinder
    // nulled the register after the first RefDec, so the second (the local's)
    // was skipped and one count leaked — the borrowed owner became undeletable.
    // Fix: the unwinder no longer nulls the register for RefDec records (each
    // aliased borrow holds its own count and must fire). Correct: after the
    // catch the owner deletes cleanly.
    TEST_CASE("F3 a ref local's borrow is released when an exception unwinds out of its frame") {
        const char* src = R"(
        struct Owner { val: i32; }
        struct E { code: i32; }
        fun E.message(): string for Exception { return "boom"; }
        fun trigger(o: ref Owner) { var r: ref Owner = o; throw E { code = 1 }; }
        fun main(): i32 {
            var a: uniq Owner = uniq Owner { val = 1 };
            try { trigger(a); } catch (e: E) { print("caught"); }
            delete a;
            print("owner deleted");
            return 0;
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.success == true);
        CHECK(r.stdout_output.find("owner deleted") != String::npos);

        // Multiple ref locals aliasing the same borrow must each be released.
        const char* multi = R"(
        struct Owner { val: i32; }
        struct E { code: i32; }
        fun E.message(): string for Exception { return "boom"; }
        fun trigger(o: ref Owner) {
            var r1: ref Owner = o;
            var r2: ref Owner = r1;
            throw E { code = 1 };
        }
        fun main(): i32 {
            var a: uniq Owner = uniq Owner { val = 1 };
            try { trigger(a); } catch (e: E) {}
            delete a;
            return 0;
        }
        )";
        CHECK(VMBackend::run(multi).success == true);

        // A uniq owner borrowed by a ref local in the same throwing frame: the
        // borrow must release before the owner's unwind Delete (no false trap).
        const char* owner_and_borrow = R"(
        struct Owner { val: i32; }
        struct E { code: i32; }
        fun E.message(): string for Exception { return "boom"; }
        fun trigger() {
            var u: uniq Owner = uniq Owner { val = 1 };
            var r: ref Owner = u;
            throw E { code = 1 };   // unwind: release r, then delete u — no trap
        }
        fun main(): i32 {
            try { trigger(); } catch (e: E) { print("ok"); }
            return 0;
        }
        )";
        auto ob = VMBackend::run(owner_and_borrow);
        CHECK(ob.success == true);
        CHECK(ob.stdout_output.find("ok") != String::npos);
    }

    // Finding 4 — FIXED. A `ref` local live across a `yield` is promoted into the
    // coroutine state struct, but the generated `$$delete` used to release only
    // ref *parameter* fields, not ref *local* fields — destroying the coroutine
    // mid-iteration leaked the borrow, leaving the owner undeletable. Fix: the
    // destructor now null-guards and RefDecs every counted ref field (params and
    // locals, excluding catch params), and a promotion pass nulls a ref local's
    // state field when its borrow is released on the resume path, so a completed
    // coroutine's teardown never double-decrements.
    TEST_CASE("F4 a coroutine's promoted ref local is released when the coroutine is destroyed") {
        const char* src = R"(
        struct Owner { val: i32; }
        fun gen(o: ref Owner): Coro<i32> {
            var r: ref Owner = o;
            yield r.val;
            yield r.val + 1;
        }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 7 };
            var c = gen(u);
            var x: i32 = c.resume();
            return x;
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.success == true);
        CHECK(r.value == 7);
    }

    // Finding 5 — `roxy_list_copy` is a raw memcpy of the element buffer, so
    // `.copy()` of a `List<uniq T>` produces two lists holding the SAME element
    // pointers → a double free (and double destructor run) at teardown. You
    // cannot duplicate a `uniq` by memcpy. Correct: reject `.copy()` on a list
    // whose elements are non-duplicable owners at compile time (or require a
    // Clone and deep-copy). Current: it compiles and double-frees at runtime
    // (a debug double-delete tripwire aborts the process — hence this asserts at
    // the compile boundary rather than running it).
    TEST_CASE("F5 .copy() of a List<uniq T> does not alias owned elements"
              * doctest::should_fail()) {
        const char* src = R"(
        struct Owner { val: i32; }
        fun delete Owner() { }
        fun main(): i32 {
            var l: List<uniq Owner> = List<uniq Owner>();
            l.push(uniq Owner { val = 1 });
            var l2: List<uniq Owner> = l.copy();   // shallow copy → both own the same ptr
            return 0;
        }
        )";
        BumpAllocator allocator(1 << 16);
        BCModule* module = compile(allocator, src);
        // Correct behavior: this program is rejected (owned elements can't be
        // duplicated by copy). It currently compiles, so `module != nullptr`.
        CHECK(module == nullptr);
    }

    // Finding 6a — `Map<_, ref V>.values()` memcpys the borrowed value pointers
    // into a fresh List<ref V> with no RefInc, so destroying that list RefDecs
    // borrows it never acquired → ref-count underflow trap. Correct: the values
    // list holds its own counted borrows and tears down cleanly. Current: traps
    // ("reference count already zero").
    TEST_CASE("F6a Map<_, ref V>.values() returns properly counted borrows"
              * doctest::should_fail()) {
        const char* src = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 5 };
            var m: Map<i32, ref Owner> = Map<i32, ref Owner>();
            m.insert(1, u);
            { var vs: List<ref Owner> = m.values(); }
            return 0;
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.success == true);
    }

    // Finding 6b — same root cause via `List<ref T>.copy()`: borrow pointers are
    // memcpy'd without RefInc, so the copy's destruction underflows the count.
    // Correct: the copy re-incs each borrow and tears down balanced. Current:
    // ref-count underflow trap.
    TEST_CASE("F6b List<ref T>.copy() re-increments each borrowed element"
              * doctest::should_fail()) {
        const char* src = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 4 };
            var l: List<ref Owner> = List<ref Owner>();
            l.push(u);
            var l2: List<ref Owner> = l.copy();
            return 0;
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.success == true);
    }

    // Finding 7 — member access on a `weak T` emits no WeakCheck (the IR op
    // exists and lowers, but the front-end never emits it), so dereferencing a
    // dangling weak is a silent stale read rather than the documented trap.
    // Correct: reading a field through a dead weak traps (there is no null to
    // return for an `i32`). Current: it silently returns stale data and the
    // program "succeeds".
    TEST_CASE("F7 dereferencing a dangling weak is detected, not a silent stale read"
              * doctest::should_fail()) {
        const char* src = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 7 };
            var w: weak Owner = u;
            delete u;
            return w.val;   // dead weak → should trap, not read stale memory
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.success == false);
    }

    // Finding 8 — a global `ref`/`weak` is uncounted (globals get no scope-exit
    // RefDec and no create-time inc), so a global ref does not block deletion of
    // its owner. Correct: `delete gu` traps while `gr` borrows it. Current: the
    // delete succeeds. Run through a bespoke harness that does NOT call
    // vm_destroy — the uncounted delete leaves the shutdown teardown to
    // double-free the global and abort, which we avoid observing here.
    TEST_CASE("F8 a global ref borrow blocks deletion of its owner"
              * doctest::should_fail()) {
        const char* src = R"(
        struct Owner { val: i32; }
        var gu: uniq Owner = uniq Owner { val = 3 };
        var gr: ref Owner = gu;
        fun main(): i32 { delete gu; return 0; }
        )";
        BumpAllocator allocator(1 << 16);
        BCModule* module = compile(allocator, src);
        REQUIRE(module != nullptr);
        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, module);
        bool ran = vm_call(&vm, StringView("main", 4), {});
        // Correct: the delete traps → vm_call returns false. Current: true.
        CHECK(ran == false);
        // NOTE: intentionally no vm_destroy(&vm)/delete module — the uncounted
        // delete makes shutdown double-free the global and abort. Leaking one VM
        // per run is acceptable for a regression marker.
    }

    // Finding 9a — a caught exception object is never freed. The catch variable
    // is a non-owning `ref`, and the handled path in the unwinder places the
    // pointer in a register without freeing it. Correct: after the handler
    // completes, the exception is freed and its destructor runs. Current: it
    // leaks (dtor never runs).
    TEST_CASE("F9a a caught exception object is freed after the handler"
              * doctest::should_fail()) {
        const char* src = R"(
        struct E { code: i32; }
        fun E.message(): string for Exception { return "boom"; }
        fun delete E() { print("E dtor"); }
        fun risky() { throw E { code = 1 }; }
        fun main(): i32 {
            try { risky(); } catch (e: E) { print("caught"); }
            return 0;
        }
        )";
        auto r = VMBackend::run(src);
        CHECK(r.stdout_output.find("caught") != String::npos);
        CHECK(r.stdout_output.find("E dtor") != String::npos);
    }

    // Finding 9b — dynamically created string temporaries (concat / f-string /
    // to_string / substr results) are never freed; they accumulate until VM
    // teardown. Correct: temporaries created and dropped in a bounded loop are
    // reclaimed, so live-object count stays bounded. Current: 200 iterations
    // leave ~600 live objects (0 tombstoned). Measured via a bespoke harness
    // reading the slab's alloc/tombstone counters before teardown.
    TEST_CASE("F9b string temporaries are reclaimed, not leaked until teardown"
              * doctest::should_fail()) {
        const char* src = R"(
        fun main(): i32 {
            var i: i32 = 0;
            while (i < 200) { var s: string = f"x{i}y"; i = i + 1; }
            return 0;
        }
        )";
        BumpAllocator allocator(1 << 20);
        BCModule* module = compile(allocator, src);
        REQUIRE(module != nullptr);
        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, module);
        REQUIRE(vm_call(&vm, StringView("main", 4), {}) == true);
        u64 live = vm.allocator->total_allocated - vm.allocator->total_tombstoned;
        // Correct: temporaries reclaimed → only a handful of live objects.
        // Current: ~600 (nothing freed).
        CHECK(live < 50);
        vm_destroy(&vm);
        delete module;
    }
}

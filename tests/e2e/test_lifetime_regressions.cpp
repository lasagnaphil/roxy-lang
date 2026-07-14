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

    // Finding 5 — FIXED. `roxy_list_copy` is a raw memcpy of the element buffer,
    // so `.copy()` of a `List<uniq T>` used to produce two lists holding the SAME
    // element pointers → a double free at teardown. You cannot duplicate a `uniq`
    // by memcpy. Fix: reject `.copy()` at compile time when the element type (or a
    // Map key/value type) is non-copyable. (`ref`/`weak` elements stay copyable —
    // the copy re-increments the borrow; see F6b.)
    TEST_CASE("F5 .copy() of a List<uniq T> does not alias owned elements") {
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
        CHECK(compile(allocator, src) == nullptr);   // rejected: uniq can't be duplicated

        // Also rejected: nested containers and Map with an owning key/value.
        auto rejected = [](const char* s) {
            BumpAllocator a(1 << 16); return compile(a, s) == nullptr;
        };
        CHECK(rejected("fun main(): i32 { var l: List<List<i32>> = List<List<i32>>();"
                       " var l2: List<List<i32>> = l.copy(); return 0; }"));
        CHECK(rejected("struct O { v: i32; } fun delete O() {}"
                       " fun main(): i32 { var m: Map<i32, uniq O> = Map<i32, uniq O>();"
                       " var m2: Map<i32, uniq O> = m.copy(); return 0; }"));

        // NOT over-rejected: copyable elements (incl. ref/weak) still allow .copy().
        auto ok = [](const char* s) {
            BumpAllocator a(1 << 16); return compile(a, s) != nullptr;
        };
        CHECK(ok("fun main(): i32 { var l: List<i32> = List<i32>();"
                 " var l2: List<i32> = l.copy(); return 0; }"));
        CHECK(ok("struct O { v: i32; } fun main(): i32 { var u: uniq O = uniq O { v = 1 };"
                 " var l: List<ref O> = List<ref O>(); l.push(u);"
                 " var l2: List<ref O> = l.copy(); return 0; }"));
    }

    // Finding 6a — FIXED. `Map<_, ref V>.values()` memcpy'd the borrowed value
    // pointers into a fresh List<ref V> with no RefInc, so destroying that list
    // RefDec'd borrows it never acquired → ref-count underflow trap. Fix:
    // roxy_map_values RefIncs each ref value (and tags the produced list so a
    // later copy re-incs). The list now holds its own counted borrows.
    TEST_CASE("F6a Map<_, ref V>.values() returns properly counted borrows") {
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

    // Finding 6b — FIXED. Same root cause via `List<ref T>.copy()`: borrow
    // pointers were memcpy'd without RefInc, so the copy's destruction underflowed
    // the count. Fix: a `List<ref T>` is tagged (element_is_ref) at construction,
    // and roxy_list_copy RefIncs each element when tagged. The copy tears down
    // balanced.
    TEST_CASE("F6b List<ref T>.copy() re-increments each borrowed element") {
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

        // The copy holds a real second borrow: deleting the owner while either
        // list is alive traps; after both are destroyed the owner is deletable.
        const char* trap = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 4 };
            var l: List<ref Owner> = List<ref Owner>();
            l.push(u);
            var l2: List<ref Owner> = l.copy();
            delete u;                 // 2 borrows outstanding → trap
            return 0;
        }
        )";
        CHECK(VMBackend::run(trap).success == false);

        const char* ok = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 4 };
            { var l: List<ref Owner> = List<ref Owner>(); l.push(u);
              var l2: List<ref Owner> = l.copy(); }
            delete u;                 // both released → deletable
            print("ok");
            return 0;
        }
        )";
        auto okr = VMBackend::run(ok);
        CHECK(okr.success == true);
        CHECK(okr.stdout_output.find("ok") != String::npos);
    }

    // Finding 7 — FIXED. Member access on a `weak T` emitted no WeakCheck (the IR
    // op existed and lowered, but the front-end never emitted it), so
    // dereferencing a dangling weak was a silent stale read. Fix: the IR builder
    // emits a WeakCheck-and-trap before any weak dereference (field read/write,
    // method call). This also surfaced (and fixed) a latent bug where a
    // `[weak self]` capture stored a bare receiver pointer with a garbage
    // generation instead of a proper WeakCreate snapshot.
    TEST_CASE("F7 dereferencing a dangling weak is detected, not a silent stale read") {
        // Read through a dead weak traps (there is no null to yield for an i32).
        const char* dead_read = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 7 };
            var w: weak Owner = u;
            delete u;
            return w.val;
        }
        )";
        CHECK(VMBackend::run(dead_read).success == false);

        // A live weak read still works and yields the real value.
        const char* live_read = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 42 };
            var w: weak Owner = u;
            return w.val;
        }
        )";
        auto lr = VMBackend::run(live_read);
        CHECK(lr.success == true);
        CHECK(lr.value == 42);

        // Write through a dead weak traps too (would land in freed memory).
        const char* dead_write = R"(
        struct Owner { val: i32; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 7 };
            var w: weak Owner = u;
            delete u;
            w.val = 9;
            return 0;
        }
        )";
        CHECK(VMBackend::run(dead_write).success == false);

        // Method call on a dead weak receiver traps.
        const char* dead_method = R"(
        struct Owner { val: i32; }
        fun Owner.get(): i32 { return self.val; }
        fun main(): i32 {
            var u: uniq Owner = uniq Owner { val = 7 };
            var w: weak Owner = u;
            delete u;
            return w.get();
        }
        )";
        CHECK(VMBackend::run(dead_method).success == false);
    }

    // Finding 8 — FIXED. Module-level globals now participate in the
    // constraint-reference model, via the synthesized __module_init /
    // __module_shutdown (ir_builder.cpp).
    //   8a: a global `ref` borrow is counted for the whole VM lifetime — RefInc
    //       in __module_init after storing the initializer, RefDec in
    //       __module_shutdown (reverse order, so before the owner's Delete). So
    //       deleting the borrowed owner traps while the global still borrows it.
    //   8b: an explicit `delete` of a `uniq` global nulls its slot, so
    //       __module_shutdown's null-guarded Delete no-ops instead of
    //       double-freeing the object already freed in user code.
    // Both are now cleanly observable through the standard harness: the 8a trap
    // sets vm->error, so vm_destroy skips shutdown; the 8b path leaves no error,
    // so shutdown runs and must not double-free. (Weak globals are generational
    // and uncounted, so they never block a delete.)
    TEST_CASE("F8 globals participate in the constraint-reference model") {
        // 8a — a global ref borrow blocks `delete gu` (traps → run fails).
        const char* borrow_trap = R"(
        struct Owner { val: i32; }
        var gu: uniq Owner = uniq Owner { val = 3 };
        var gr: ref Owner = gu;
        fun main(): i32 { delete gu; return 0; }
        )";
        CHECK(VMBackend::run(borrow_trap).success == false);

        // Control: with the borrow confined to a shorter-lived reader (no global
        // ref outstanding), deleting the owner succeeds.
        const char* no_borrow = R"(
        struct Owner { val: i32; }
        var gu: uniq Owner = uniq Owner { val = 3 };
        fun main(): i32 { delete gu; return 0; }
        )";
        auto nbr = VMBackend::run(no_borrow);
        CHECK(nbr.success == true);
        CHECK(nbr.value == 0);

        // 8b — a uniq global deleted in user code is destroyed exactly once
        // *there* (its dtor prints within main's captured window), and is NOT
        // deleted again at shutdown: the un-fixed double-free path re-ran the
        // dtor on a tombstoned slot and aborted on the debug double-delete
        // tripwire, so a clean run with a single "dtor" line is the fix.
        const char* deleted_in_main = R"(
        struct Owner { val: i32; }
        fun delete Owner() { print("dtor"); }
        var gu: uniq Owner = uniq Owner { val = 3 };
        fun main(): i32 { delete gu; return 0; }
        )";
        auto dmr = VMBackend::run(deleted_in_main);
        CHECK(dmr.success == true);
        CHECK(dmr.stdout_output == "dtor\n");

        // A uniq global left alive is still torn down at shutdown (the 8b
        // null-on-delete must not break the normal RAII path). Its dtor runs at
        // vm_destroy, past run_and_capture's window, so assert clean teardown
        // rather than the text (cf. test_globals.cpp).
        const char* alive_to_shutdown = R"(
        struct Owner { val: i32; }
        fun delete Owner() {}
        var gu: uniq Owner = uniq Owner { val = 3 };
        fun main(): i32 { return gu.val; }
        )";
        auto air = VMBackend::run(alive_to_shutdown);
        CHECK(air.success == true);
        CHECK(air.value == 3);

        // Global-weak sanity: a `weak` global does NOT block deletion of its
        // owner (weak is uncounted/generational)...
        const char* weak_no_block = R"(
        struct Owner { val: i32; }
        var gu: uniq Owner = uniq Owner { val = 5 };
        var gw: weak Owner = gu;
        fun main(): i32 { delete gu; return 0; }
        )";
        CHECK(VMBackend::run(weak_no_block).success == true);

        // ...but dereferencing it after the owner dies still traps via WeakCheck
        // (finding 7), not a silent dangling read — the check is type-driven, so
        // it fires for a global weak just as for a local one.
        const char* weak_dangling = R"(
        struct Owner { val: i32; }
        var gu: uniq Owner = uniq Owner { val = 5 };
        var gw: weak Owner = gu;
        fun main(): i32 { delete gu; return gw.val; }
        )";
        CHECK(VMBackend::run(weak_dangling).success == false);
    }

    // Finding 9a — FIXED. A caught exception object used to leak: the catch
    // variable is a non-owning `ref`, and the handled path in the unwinder placed
    // the pointer in a register without ever freeing it. Fix: the caught exception
    // is registered as an owned local of its catch scope, so the ordinary
    // scope-cleanup machinery frees it on every exit (fall-through, return, break,
    // continue, and a *new* throw unwinding out of the catch); a re-throw hands it
    // off, guarded by the in-flight check in object_free / delete_value (VM) and
    // roxy_exception_current() (C backend), so it is freed exactly once. Richer
    // dtor-ordering coverage — re-throw, new-throw-in-catch, return/finally,
    // catch-all reclamation — lives in the `E2E Exceptions` suite on both backends.
    TEST_CASE("F9a a caught exception object is freed after the handler") {
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

    // Finding 9b — FIXED. Dynamically created string temporaries (concat /
    // f-string / to_string / substr results) used to leak until VM teardown.
    // Strings are now reference-counted (an owner count in the object header;
    // pooled literals are immortal): a copy retains, a drop releases, and the last
    // release frees. So temporaries created and dropped in a bounded loop are
    // reclaimed and the live-object count stays bounded. Measured via a bespoke
    // harness reading the slab's alloc/tombstone counters before teardown.
    //
    // Scope note: standalone strings and container (List/Map) string elements are
    // reclaimed; strings held in *struct fields* are retained-on-store (so they
    // never dangle) but not released on struct drop — structs stay copyable and
    // trivial, so a string in a struct field is a bounded leak, as before. This
    // loop exercises the standalone/temporary path, which is fully reclaimed.
    TEST_CASE("F9b string temporaries are reclaimed, not leaked until teardown") {
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
        REQUIRE(vm_call(&vm, "main"_sv, {}) == true);
        u64 live = vm.allocator->total_allocated - vm.allocator->total_tombstoned;
        // Reclaimed → only a handful of live objects (was ~600, nothing freed).
        CHECK(live < 50);
        vm_destroy(&vm);
        delete module;
    }

    // Finding 9b (container elements) — a List<string> owns its elements: push
    // retains, and destroy releases each via the StrRelease element descriptor.
    // Building and dropping many single-element lists in a loop reclaims both the
    // list buffers and the string elements, so the live count stays bounded.
    TEST_CASE("string elements of a List are reclaimed when the list drops") {
        const char* src = R"(
        fun main(): i32 {
            var i: i32 = 0;
            while (i < 200) {
                var l: List<string> = List<string>();
                l.push(f"item{i}");
                i = i + 1;
            }
            return 0;
        }
        )";
        BumpAllocator allocator(1 << 20);
        BCModule* module = compile(allocator, src);
        REQUIRE(module != nullptr);
        RoxyVM vm;
        vm_init(&vm);
        vm_load_module(&vm, module);
        REQUIRE(vm_call(&vm, "main"_sv, {}) == true);
        u64 live = vm.allocator->total_allocated - vm.allocator->total_tombstoned;
        CHECK(live < 50);
        vm_destroy(&vm);
        delete module;
    }
}

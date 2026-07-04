#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// Module-level global variables. A top-level `var` gets persistent storage in
// the VM's global slot array, is initialized by a synthesized `__module_init`
// (constructors included) before any user call, accessed via GLOBAL_ADDR, and —
// for noncopyable types — torn down by `__module_shutdown` at VM destroy.
TEST_SUITE("E2E Globals") {

    // A primitive global compiles, initializes, and is readable from main.
    TEST_CASE_TEMPLATE("primitive global initializes and reads", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var n: i32 = 42;
            fun main(): i32 { return n; }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    // Globals are mutable and persist across function calls.
    TEST_CASE_TEMPLATE("global mutation persists across calls", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var counter: i32 = 0;
            fun bump() { counter = counter + 1; }
            fun main(): i32 {
                bump();
                bump();
                bump();
                return counter;   // 3
            }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 3);
    }

    // Multiple globals get distinct slots (no aliasing).
    TEST_CASE_TEMPLATE("multiple globals are independent", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var a: i32 = 10;
            var b: i64 = 20l;     // 2 slots — exercises offset accounting
            var c: i32 = 30;
            fun main(): i32 {
                a = a + c;                 // 40
                return a + i32(b);         // 60
            }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 60);
    }

    // A later global's initializer can read an earlier global (init runs in
    // declaration order).
    TEST_CASE_TEMPLATE("global initializer reads an earlier global", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var base: i32 = 100;
            var derived: i32 = base + 5;
            fun main(): i32 { return derived; }   // 105
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 105);
    }

    // A function-local of the same name shadows the global.
    TEST_CASE_TEMPLATE("local shadows global", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var x: i32 = 1;
            fun main(): i32 {
                var x: i32 = 99;   // shadows the global
                return x;          // 99, not 1
            }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 99);
    }

    // A global is readable from any function, not just main.
    TEST_CASE_TEMPLATE("global readable from a helper function", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            var scale: i32 = 7;
            fun scaled(v: i32): i32 { return v * scale; }
            fun main(): i32 { return scaled(6); }   // 42
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 42);
    }

    // Struct-typed global: fields are read/written through the global's address.
    TEST_CASE_TEMPLATE("struct global field access", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            struct Point { x: i32; y: i32; }
            var origin: Point = Point { x = 3, y = 4 };
            fun main(): i32 {
                origin.x = origin.x + 10;
                return origin.x + origin.y;   // 13 + 4 = 17
            }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 17);
    }

    // A `uniq` global runs its constructor at init (the original bug) — the field
    // set by the constructor is visible, and the program tears down cleanly at
    // shutdown (no double-free; the debug double-delete tripwire would fire
    // otherwise).
    TEST_CASE_TEMPLATE("uniq global runs its constructor and reads back", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            struct Counter { value: i32; }
            fun new Counter(v: i32) { self.value = v; }
            fun delete Counter() {}
            var g: uniq Counter = uniq Counter(7);
            fun main(): i32 { return g.value; }   // 7 (constructor ran)
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 7);
    }

    // A `uniq` global destructor runs at shutdown. The destructor prints, which
    // lands on real stdout *after* run_and_capture restores it (global teardown
    // is at vm_destroy, past the captured window), so this asserts the program
    // runs and tears down without error rather than checking the text.
    TEST_CASE_TEMPLATE("uniq global with destructor tears down cleanly", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            struct Resource { id: i32; }
            fun new Resource(id: i32) { self.id = id; }
            fun delete Resource() { print("freed"); }
            var res: uniq Resource = uniq Resource(1);
            fun main(): i32 { return res.id; }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 1);
    }

    // Reassigning a `uniq` global frees the overwritten object (mirrors the
    // inout/field old-value destroy), so init + reassign + shutdown is a single
    // clean chain with no leak and no double-free.
    TEST_CASE_TEMPLATE("uniq global reassignment frees the old value", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
            struct Box { v: i32; }
            fun new Box(v: i32) { self.v = v; }
            fun delete Box() {}
            var b: uniq Box = uniq Box(1);
            fun main(): i32 {
                b = uniq Box(2);   // frees Box(1), stores Box(2)
                return b.v;        // 2
            }
        )";
        auto result = Backend::run(source);
        CHECK(result.success == true);
        CHECK(result.value == 2);
    }

    // Finding 8 (lifetime audit) — module-level globals participate in the
    // constraint-reference model. A global `ref` is counted for the whole VM
    // lifetime (RefInc in __module_init after the initializer is stored, RefDec
    // in __module_shutdown), and an explicit `delete` of a `uniq` global nulls
    // its slot so shutdown's null-guarded Delete no-ops instead of double-freeing.
    // Both backends lower the same synthesized init/shutdown IR. The
    // delete-while-borrowed *trap* is asserted VM-only (test_lifetime_regressions
    // "F8*"): the C backend renders that trap as a raw abort, not a catchable
    // error.
    TEST_CASE_TEMPLATE("global ref is counted; deleted uniq global isn't double-freed",
                       Backend, RX_E2E_BACKENDS) {
        // A global ref reads through to its owner; the init RefInc and shutdown
        // RefDec balance, so the owner tears down cleanly (a missing dec would
        // leave the owner borrowed at its shutdown Delete).
        const char* read_through = R"(
            struct Owner { val: i32; }
            var gu: uniq Owner = uniq Owner { val = 42 };
            var gr: ref Owner = gu;
            fun main(): i32 { return gr.val; }
        )";
        auto rt = Backend::run(read_through);
        CHECK(rt.success == true);
        CHECK(rt.value == 42);

        // Deleting a uniq global in user code nulls its slot, so __module_shutdown
        // does not free it a second time at teardown.
        const char* delete_in_main = R"(
            struct Owner { val: i32; }
            fun delete Owner() {}
            var gu: uniq Owner = uniq Owner { val = 7 };
            fun main(): i32 { delete gu; return 0; }
        )";
        auto dm = Backend::run(delete_in_main);
        CHECK(dm.success == true);
        CHECK(dm.value == 0);
    }
}

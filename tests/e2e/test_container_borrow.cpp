#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Container borrows: `ref List<T>` / `ref Map<K, V>`
// ============================================================================
//
// A container value is a pointer to a slab-allocated header, so borrowing one
// is `uniq -> ref` with a different pointee: same thin pointer, same
// ObjectHeader.ref_count, same free-trap. Before this existed a container
// parameter was either owning (`List<T>`, which *moves* the caller's value) or
// `inout` — so a function that only reads its argument had to advertise
// mutation and could not be given the same container twice.
//
// The counting is the generic `ref`-parameter machinery (RefInc at entry,
// RefDec on every exit path incl. unwind), so these tests are as much about the
// balance as about the binding: an unbalanced borrow shows up as a trap at the
// owner's drop, not as a wrong number.

TEST_SUITE("E2E Container Borrow") {

    TEST_CASE_TEMPLATE("read-only List borrow leaves the caller owning", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun total(xs: ref List<i32>): i32 {
            var t: i32 = 0;
            for (var i: i32 = 0; i < xs.len(); i = i + 1) { t = t + xs[i]; }
            return t;
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2); xs.push(3);
            print(f"{total(xs)}");
            print(f"{total(xs)}");
            print(f"{xs.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // The borrow does not consume: `xs` is still callable a second time and
        // still live afterwards. Passing it by value would move it.
        CHECK(result.stdout_output == "6\n6\n3\n");
    }

    TEST_CASE_TEMPLATE("a List borrow is mutable through the borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun fill(xs: ref List<i32>) {
            xs.push(10);
            xs.push(20);
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            fill(xs);
            print(f"{xs}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // `ref` is a borrow, not an immutable borrow — it names the object, so
        // mutating through it is legitimate (what it can't do is reassign the
        // caller's slot; that's `inout`).
        CHECK(result.stdout_output == "[1, 10, 20]\n");
    }

    TEST_CASE_TEMPLATE("the same container can be borrowed twice at once", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun compare(a: ref List<i32>, b: ref List<i32>): i32 {
            return a.len() * 10 + b.len();
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            print(f"{compare(xs, xs)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // The aliasing an `inout` parameter would forbid. Two live borrows put
        // ref_count at 2 inside the call; both decrement on return.
        CHECK(result.stdout_output == "22\n");
    }

    TEST_CASE_TEMPLATE("read-only Map borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun lookup(m: ref Map<string, i32>, k: string): i32 {
            return m.get_or(k, -1);
        }

        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            m.insert("a", 1);
            m.insert("b", 2);
            print(f"{lookup(m, "a")}");
            print(f"{lookup(m, "zz")}");
            print(f"{m.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n-1\n2\n");
    }

    TEST_CASE_TEMPLATE("a Map borrow is mutable through the borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun bump(m: ref Map<string, i32>, k: string) {
            m.insert(k, m.get_or(k, 0) + 1);
        }

        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            bump(m, "hits");
            bump(m, "hits");
            bump(m, "hits");
            print(f"{m.get("hits")}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "3\n");
    }

    TEST_CASE_TEMPLATE("a borrow passes onward as a borrow", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun inner(xs: ref List<i32>): i32 { return xs.len(); }
        fun middle(xs: ref List<i32>): i32 { return inner(xs) + inner(xs); }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            print(f"{middle(xs)}");
            print(f"{xs.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "4\n2\n");
    }

    TEST_CASE_TEMPLATE("an inout container reaches a borrow parameter", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun peek(xs: ref List<i32>): i32 { return xs.len(); }

        fun grow(xs: inout List<i32>): i32 {
            xs.push(9);
            return peek(xs);
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            print(f"{grow(inout xs)}");
            print(f"{xs}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // Sound in a way the struct case is not: a container's pointee is
        // always heap, so the borrow has a real ObjectHeader to count on.
        CHECK(result.stdout_output == "2\n[1, 9]\n");
    }

    TEST_CASE_TEMPLATE("a struct's container field can be borrowed", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Bag { items: List<i32>; }

        fun total(xs: ref List<i32>): i32 {
            var t: i32 = 0;
            for (var i: i32 = 0; i < xs.len(); i = i + 1) { t = t + xs[i]; }
            return t;
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(4); xs.push(5);
            var bag: Bag = Bag { items = xs };
            print(f"{total(bag.items)}");
            print(f"{bag.items}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // Borrowing the field must not move it out of the struct.
        CHECK(result.stdout_output == "9\n[4, 5]\n");
    }

    TEST_CASE_TEMPLATE("a borrowed container is Printable", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun show(xs: ref List<i32>, m: ref Map<string, i32>) {
            print(f"{xs}");
            print(xs);
            print(f"{m}");
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            var m: Map<string, i32> = Map<string, i32>();
            m.insert("k", 7);
            show(xs, m);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // The `inout` form a read-only function used to be forced into could
        // already do this (its param type is the bare container); switching to
        // a borrow must not lose it.
        CHECK(result.stdout_output == "[1, 2]\n[1, 2]\n{k: 7}\n");
    }

    TEST_CASE_TEMPLATE("a ref local borrows a container", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            var r: ref List<i32> = xs;
            r.push(2);
            print(f"{r[0]}");
            print(f"{r.len()}");
            print(f"{xs}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n[1, 2]\n");
    }

    TEST_CASE_TEMPLATE("an element of a borrowed List is an inout lvalue", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun add_to(x: inout i32, n: i32) { x = x + n; }

        fun scale(xs: ref List<i32>) {
            for (var i: i32 = 0; i < xs.len(); i = i + 1) { add_to(inout xs[i], 100); }
        }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            scale(xs);
            print(f"{xs}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // The element-borrow pin composes with the container borrow: the pin's
        // borrow_count and the borrow's ref_count are separate counters.
        CHECK(result.stdout_output == "[101, 102]\n");
    }

    TEST_CASE_TEMPLATE("borrow counting is balanced across a loop", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun peek(xs: ref List<i32>): i32 { return xs.len(); }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            var t: i32 = 0;
            for (var i: i32 = 0; i < 200; i = i + 1) { t = t + peek(xs); }
            print(f"{t}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // A missed decrement would leave ref_count at 200 and make `xs`'s
        // scope-exit drop trap, so reaching the end at all is the assertion.
        CHECK(result.stdout_output == "200\n");
    }

    TEST_CASE_TEMPLATE("borrow counting is balanced across an unwind", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Boom { code: i32; }
        fun Boom.message(): string for Exception { return "boom"; }

        fun thrower(xs: ref List<i32>): i32 { throw Boom { code = 1 }; }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1);
            try {
                thrower(xs);
            } catch (e: Boom) {
                print("caught");
            }
            print(f"{xs}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // Throwing *out of* a borrowing frame must still decrement, or `xs`'s
        // drop traps on a borrow that no longer exists.
        CHECK(result.stdout_output == "caught\n[1]\n");
    }

    TEST_CASE_TEMPLATE("a borrow of a List of structs", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Point { x: i32; y: i32; }

        fun sum_x(pts: ref List<Point>): i32 {
            var t: i32 = 0;
            for (var i: i32 = 0; i < pts.len(); i = i + 1) { t = t + pts[i].x; }
            return t;
        }

        fun main(): i32 {
            var pts: List<Point> = List<Point>();
            pts.push(Point { x = 1, y = 2 });
            pts.push(Point { x = 30, y = 4 });
            print(f"{sum_x(pts)}");
            print(f"{pts.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "31\n2\n");
    }

    TEST_CASE_TEMPLATE("a borrow reaches a generic function", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun count<T>(xs: ref List<T>): i32 { return xs.len(); }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            var ys: List<string> = List<string>();
            ys.push("a");
            print(f"{count(xs)}");
            print(f"{count(ys)}");
            print(f"{xs.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n1\n2\n");
    }

    TEST_CASE_TEMPLATE("a borrowed container can be copied but not moved", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun clone(xs: ref List<i32>): List<i32> { return xs.copy(); }

        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            xs.push(1); xs.push(2);
            var ys: List<i32> = clone(xs);
            ys.push(3);
            print(f"{xs}");
            print(f"{ys}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        // `.copy()` reads through the borrow and produces an independent owner;
        // `return xs;` (a move out of a borrow) is the rejection tested below.
        CHECK(result.stdout_output == "[1, 2]\n[1, 2, 3]\n");
    }

    // ---- Rejections ----------------------------------------------------

    TEST_CASE("a borrow cannot be moved out of the borrowing frame") {
        const char* source = R"(
        fun steal(xs: ref List<i32>): List<i32> { return xs; }
        fun main(): i32 { return 0; }
    )";

        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    TEST_CASE("a borrow cannot be passed as an inout argument") {
        const char* source = R"(
        fun replace(xs: inout List<i32>) { xs = List<i32>(); }
        fun outer(xs: ref List<i32>) { replace(inout xs); }
        fun main(): i32 { return 0; }
    )";

        // `inout` reassigns the caller's slot; a borrow names the object and
        // has no slot to give.
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    TEST_CASE("a List borrow does not convert to a differently-parameterized one") {
        const char* source = R"(
        fun take(xs: ref List<i64>): i32 { return xs.len(); }
        fun main(): i32 {
            var xs: List<i32> = List<i32>();
            return take(xs);
        }
    )";

        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    TEST_CASE("a stack value struct still does not convert to a borrow") {
        const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun take(p: ref Point): i32 { return p.x; }
        fun main(): i32 {
            var p: Point = Point { x = 1, y = 2 };
            return take(p);
        }
    )";

        // Containers are always heap, which is why borrowing one is sound. That
        // must not have loosened the rule for stack pointees.
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

    TEST_CASE("a weak container reference is still not Printable") {
        const char* source = R"(
        struct Point { x: i32; y: i32; }
        fun Point.to_string(): string for Printable { return "pt"; }
        fun main(): i32 {
            var p: uniq Point = uniq Point();
            var w: weak Point = p;
            print(f"{w}");
            return 0;
        }
    )";

        // uniq/ref print as their pointee; `weak` can dangle, so it does not.
        BumpAllocator allocator(65536);
        CHECK(compile(allocator, source) == nullptr);
    }

}  // TEST_SUITE("E2E Container Borrow")

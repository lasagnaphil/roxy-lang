#include "roxy/core/doctest/doctest.h"

#include "roxy/rt/roxy_rt.h"

#include <cstdint>

// Phase 1 of the container element-lvalue design (docs/internals/lifetimes.md
// §15): the runtime *pin* mechanism. An element borrow pins its container; while
// pinned, structural mutators (those that realloc/rehash/shift/free the backing
// buffer) refuse the mutation and raise the runtime lifetime-violation trap, so a
// borrowed element pointer can never be dangled. These tests exercise the shared
// runtime directly at the C-API level — the VM / C-backend wiring is later phases.

TEST_SUITE("Container Pin") {

    TEST_CASE("list pin refuses structural mutation; unpin restores it") {
        roxy_ctx ctx;
        roxy_ctx_init(&ctx);
        {
            roxy::ScopedContext guard(&ctx);
            roxy_runtime_error_clear();

            // List<i32>: 1 u32 slot per element, inline.
            void* lst = roxy_list_alloc(1, 1);
            REQUIRE(lst != nullptr);
            roxy_list_init(lst, 0);
            for (uint32_t i = 0; i < 3; i++) roxy_list_push(lst, &i);  // [0,1,2]
            REQUIRE(roxy_list_len(lst) == 3);
            REQUIRE(!roxy_runtime_error_pending());

            // Pin (simulating an outstanding `inout list[i]` borrow).
            roxy_list_pin(lst);

            // push may realloc → refused while pinned: list unchanged, trap raised.
            uint32_t v = 99;
            roxy_list_push(lst, &v);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_runtime_error_message() != nullptr);
            CHECK(roxy_list_len(lst) == 3);  // push did not take effect

            roxy_runtime_error_clear();

            // In-place ops stay allowed while pinned (no realloc): set + reads.
            uint32_t set_val = 42;
            roxy_list_set(lst, 1, &set_val);
            CHECK(!roxy_runtime_error_pending());
            CHECK(*static_cast<uint32_t*>(roxy_list_get(lst, 1)) == 42);
            CHECK(roxy_list_len(lst) == 3);
            CHECK(!roxy_runtime_error_pending());

            // Unpin → push works again.
            roxy_list_unpin(lst);
            roxy_list_push(lst, &v);
            CHECK(!roxy_runtime_error_pending());
            CHECK(roxy_list_len(lst) == 4);
            CHECK(*static_cast<uint32_t*>(roxy_list_get(lst, 3)) == 99);

            roxy_list_delete(lst);
            roxy_free(lst);
            roxy_runtime_error_clear();
        }
        roxy_ctx_destroy(&ctx);
    }

    TEST_CASE("list pin nests (count, not flag)") {
        roxy_ctx ctx;
        roxy_ctx_init(&ctx);
        {
            roxy::ScopedContext guard(&ctx);
            roxy_runtime_error_clear();

            void* lst = roxy_list_alloc(1, 1);
            REQUIRE(lst != nullptr);
            roxy_list_init(lst, 0);
            uint32_t zero = 0;
            roxy_list_push(lst, &zero);
            REQUIRE(roxy_list_len(lst) == 1);

            roxy_list_pin(lst);
            roxy_list_pin(lst);   // two outstanding borrows

            uint32_t v = 7;
            roxy_list_unpin(lst);  // one released — still pinned
            roxy_list_push(lst, &v);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_list_len(lst) == 1);
            roxy_runtime_error_clear();

            roxy_list_unpin(lst);  // last released — unpinned
            roxy_list_push(lst, &v);
            CHECK(!roxy_runtime_error_pending());
            CHECK(roxy_list_len(lst) == 2);

            roxy_list_delete(lst);
            roxy_free(lst);
            roxy_runtime_error_clear();
        }
        roxy_ctx_destroy(&ctx);
    }

    TEST_CASE("a copy of a pinned list is not itself pinned") {
        roxy_ctx ctx;
        roxy_ctx_init(&ctx);
        {
            roxy::ScopedContext guard(&ctx);
            roxy_runtime_error_clear();

            void* lst = roxy_list_alloc(1, 1);
            REQUIRE(lst != nullptr);
            roxy_list_init(lst, 0);
            uint32_t a = 1;
            roxy_list_push(lst, &a);

            roxy_list_pin(lst);                 // original pinned
            void* copy = roxy_list_copy(lst);   // fresh object, borrow_count = 0
            REQUIRE(copy != nullptr);

            uint32_t b = 2;
            roxy_list_push(copy, &b);           // copy is not pinned → works
            CHECK(!roxy_runtime_error_pending());
            CHECK(roxy_list_len(copy) == 2);

            // ...while the original is still frozen.
            roxy_list_push(lst, &b);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_list_len(lst) == 1);
            roxy_runtime_error_clear();

            roxy_list_unpin(lst);
            roxy_list_delete(lst);
            roxy_free(lst);
            roxy_list_delete(copy);
            roxy_free(copy);
            roxy_runtime_error_clear();
        }
        roxy_ctx_destroy(&ctx);
    }

    TEST_CASE("map pin refuses insert/remove/clear; reads stay allowed") {
        roxy_ctx ctx;
        roxy_ctx_init(&ctx);
        {
            roxy::ScopedContext guard(&ctx);
            roxy_runtime_error_clear();

            // Map<i64,i64>: primitive keys/values are 2 u32 slots (a u64), inline.
            void* m = roxy_map_alloc(2, 1, 2, 1, nullptr, nullptr);
            REQUIRE(m != nullptr);
            roxy_map_init(m, ROXY_MAP_KEY_INTEGER, 0);
            uint64_t k1 = 1, v1 = 100;
            roxy_map_insert(m, &k1, &v1);
            REQUIRE(roxy_map_len(m) == 1);

            roxy_map_pin(m);

            // insert (may rehash) refused; map unchanged.
            uint64_t k2 = 2, v2 = 200;
            roxy_map_insert(m, &k2, &v2);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_map_len(m) == 1);
            roxy_runtime_error_clear();

            // remove (backward-shift) refused; entry still present.
            CHECK(roxy_map_remove(m, &k1) == false);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_map_contains(m, &k1));
            roxy_runtime_error_clear();

            // clear refused; map unchanged.
            roxy_map_clear(m);
            CHECK(roxy_runtime_error_pending());
            CHECK(roxy_map_len(m) == 1);
            roxy_runtime_error_clear();

            // Reads stay allowed while pinned.
            CHECK(roxy_map_contains(m, &k1));
            CHECK(*static_cast<uint64_t*>(roxy_map_get(m, &k1)) == 100);
            CHECK(!roxy_runtime_error_pending());

            // Unpin → insert works again.
            roxy_map_unpin(m);
            roxy_map_insert(m, &k2, &v2);
            CHECK(!roxy_runtime_error_pending());
            CHECK(roxy_map_len(m) == 2);

            roxy_map_delete(m);
            roxy_free(m);
            roxy_runtime_error_clear();
        }
        roxy_ctx_destroy(&ctx);
    }
}

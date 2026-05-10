#include "roxy/core/doctest/doctest.h"

#include "roxy/rt/roxy_rt.h"

TEST_CASE("Runtime ctx - init zero-initializes fields") {
    roxy_ctx ctx;
    ctx.allocator = reinterpret_cast<roxy_allocator*>(0xdeadbeefULL);
    ctx.exception_state = reinterpret_cast<void*>(0xdeadbeefULL);
    ctx.user_data = reinterpret_cast<void*>(0xdeadbeefULL);

    roxy_ctx_init(&ctx);

    CHECK(ctx.allocator == nullptr);
    CHECK(ctx.exception_state == nullptr);
    CHECK(ctx.user_data == nullptr);
}

TEST_CASE("Runtime ctx - set/get round-trip") {
    roxy_set_ctx(nullptr);
    CHECK(roxy_get_ctx() == nullptr);

    roxy_ctx ctx;
    roxy_ctx_init(&ctx);
    roxy_set_ctx(&ctx);
    CHECK(roxy_get_ctx() == &ctx);

    roxy_set_ctx(nullptr);
    CHECK(roxy_get_ctx() == nullptr);
    roxy_ctx_destroy(&ctx);
}

TEST_CASE("Runtime ctx - ScopedContext restores previous on scope exit") {
    roxy_set_ctx(nullptr);

    roxy_ctx outer;
    roxy_ctx_init(&outer);
    roxy_set_ctx(&outer);
    REQUIRE(roxy_get_ctx() == &outer);

    {
        roxy_ctx inner;
        roxy_ctx_init(&inner);
        roxy::ScopedContext guard(&inner);
        CHECK(roxy_get_ctx() == &inner);
        roxy_ctx_destroy(&inner);
    }

    CHECK(roxy_get_ctx() == &outer);

    roxy_set_ctx(nullptr);
    roxy_ctx_destroy(&outer);
}

TEST_CASE("Runtime ctx - ScopedContext nests") {
    roxy_set_ctx(nullptr);

    roxy_ctx a;
    roxy_ctx b;
    roxy_ctx c;
    roxy_ctx_init(&a);
    roxy_ctx_init(&b);
    roxy_ctx_init(&c);

    {
        roxy::ScopedContext g1(&a);
        CHECK(roxy_get_ctx() == &a);
        {
            roxy::ScopedContext g2(&b);
            CHECK(roxy_get_ctx() == &b);
            {
                roxy::ScopedContext g3(&c);
                CHECK(roxy_get_ctx() == &c);
            }
            CHECK(roxy_get_ctx() == &b);
        }
        CHECK(roxy_get_ctx() == &a);
    }
    CHECK(roxy_get_ctx() == nullptr);

    roxy_ctx_destroy(&a);
    roxy_ctx_destroy(&b);
    roxy_ctx_destroy(&c);
}

TEST_CASE("Runtime ctx - user_data is settable for embedder use") {
    int sentinel = 42;

    roxy_ctx ctx;
    roxy_ctx_init(&ctx);
    ctx.user_data = &sentinel;

    roxy::ScopedContext guard(&ctx);
    auto* current = roxy_get_ctx();
    REQUIRE(current != nullptr);
    CHECK(current->user_data == &sentinel);
    CHECK(*static_cast<int*>(current->user_data) == 42);

    roxy_ctx_destroy(&ctx);
}

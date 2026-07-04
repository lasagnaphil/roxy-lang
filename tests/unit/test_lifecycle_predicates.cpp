#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/types.hpp"

using namespace rx;

// Migration step 1 of the lifecycle-traits design
// (docs/internals/lifetimes.md §18): the structural value-lifecycle predicates
// is_copy / needs_drop / needs_retain / is_trivial. They have no behavioral
// consumers yet — they are the source of truth the future drop/copy/clone glue
// lowering will use; here we just pin their definitions. Struct behavior with
// *synthesized* destructors (which require the semantic pass) is exercised
// end-to-end by the build_delete_desc cross-check assertion across the e2e suite.

TEST_SUITE("Lifecycle Predicates") {

    TEST_CASE("primitives are trivial") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* prims[] = { types.i32_type(), types.i64_type(), types.bool_type(),
                          types.f64_type() };
        for (Type* t : prims) {
            CHECK(t->is_copy());
            CHECK_FALSE(t->needs_drop());
            CHECK_FALSE(t->needs_retain());
            CHECK(t->is_trivial());
        }
    }

    TEST_CASE("string is reference-counted: copyable, but drops and retains") {
        // Finding 9b: `string` is a copyable value whose copies retain and whose
        // drops release (freeing at zero), like `ref` — not a trivial primitive.
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* s = types.string_type();
        CHECK(s->is_copy());        // freely (implicitly) copyable
        CHECK(s->needs_drop());     // ... but each drop must release
        CHECK(s->needs_retain());   // ... and each copy must retain
        CHECK_FALSE(s->is_trivial());
    }

    TEST_CASE("ref is a counted borrow: copyable, but drops and retains") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* r = types.ref_type(types.i32_type());
        CHECK(r->is_copy());        // a ref is freely (implicitly) copyable
        CHECK(r->needs_drop());     // ... but each drop must ref_dec
        CHECK(r->needs_retain());   // ... and each copy must ref_inc
        CHECK_FALSE(r->is_trivial());
    }

    TEST_CASE("weak is copyable and trivial (generation-based, no count)") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* w = types.weak_type(types.i32_type());
        CHECK(w->is_copy());
        CHECK_FALSE(w->needs_drop());
        CHECK_FALSE(w->needs_retain());
        CHECK(w->is_trivial());
    }

    TEST_CASE("uniq is move-only and drops") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* u = types.uniq_type(types.i32_type());
        CHECK_FALSE(u->is_copy());
        CHECK(u->needs_drop());
        CHECK_FALSE(u->needs_retain());  // move-only → no implicit-copy path
        CHECK_FALSE(u->is_trivial());
    }

    TEST_CASE("containers are move-only and drop") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* lst = types.list_type(types.i32_type());
        Type* m = types.map_type(types.i32_type(), types.i32_type());
        Type* m_refval = types.map_type(types.i32_type(), types.ref_type(types.i32_type()));
        for (Type* c : { lst, m, m_refval }) {
            CHECK_FALSE(c->is_copy());
            CHECK(c->needs_drop());
            CHECK_FALSE(c->needs_retain());  // the container itself is not a borrow
            CHECK_FALSE(c->is_trivial());
        }
    }

    TEST_CASE("coroutine is move-only and drops") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* co = types.coroutine_type(types.i32_type());
        CHECK_FALSE(co->is_copy());
        CHECK(co->needs_drop());
        CHECK_FALSE(co->is_trivial());
    }

    TEST_CASE("enum is trivial") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* e = types.enum_type(StringView("E", 1), nullptr);
        CHECK(e->is_copy());
        CHECK_FALSE(e->needs_drop());
        CHECK(e->is_trivial());
    }

    // The novel case the design adds (and the current descriptor misses): a
    // *copyable* struct holding a `ref` field must still drop (ref_dec) and retain
    // (ref_inc on copy). No synthesized destructor is involved — the field walk
    // catches it directly. (Type uses a union for per-kind info, so we must
    // initialize every span the predicates walk.)
    TEST_CASE("copyable struct holding a ref field drops and retains") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);

        Type* has_ref = types.struct_type(StringView("HasRef", 6), nullptr);
        auto* rf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        rf[0] = FieldInfo{ StringView("r", 1), types.ref_type(types.i32_type()),
                           /*is_pub=*/true, /*index=*/0, /*slot_offset=*/0, /*slot_count=*/2 };
        has_ref->struct_info.fields = Span<FieldInfo>(rf, 1);
        has_ref->struct_info.when_clauses = Span<WhenClauseInfo>();
        has_ref->struct_info.destructors = Span<DestructorInfo>();

        CHECK(has_ref->is_copy());        // no destructor synthesized → still copyable
        CHECK(has_ref->needs_drop());     // ... but the ref field must be released
        CHECK(has_ref->needs_retain());   // ... and re-borrowed on copy
        CHECK_FALSE(has_ref->is_trivial());

        // A struct of only plain fields stays trivial.
        Type* plain = types.struct_type(StringView("Plain", 5), nullptr);
        auto* pf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo) * 2, alignof(FieldInfo)));
        pf[0] = FieldInfo{ StringView("x", 1), types.i32_type(), true, 0, 0, 1 };
        pf[1] = FieldInfo{ StringView("y", 1), types.f64_type(), true, 1, 1, 2 };
        plain->struct_info.fields = Span<FieldInfo>(pf, 2);
        plain->struct_info.when_clauses = Span<WhenClauseInfo>();
        plain->struct_info.destructors = Span<DestructorInfo>();

        CHECK(plain->is_copy());
        CHECK_FALSE(plain->needs_drop());
        CHECK(plain->is_trivial());
    }
}

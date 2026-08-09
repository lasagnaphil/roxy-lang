#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/types/types.hpp"

using namespace rx;

// The structural value-lifecycle decisions (docs/internals/lifetimes.md → "The
// value lifecycle"): the is_copy / needs_drop / needs_retain / is_trivial
// predicates, and the two *plans* that codegen consumes — compute_drop_plan and
// compute_retain_plan.
//
// The predicates still have no code-emitting consumer; the plans are the source
// of truth. compute_drop_plan is fully wired (both backends, plus
// member_needs_drop), and compute_retain_plan now drives the clone glue at every
// struct duplication site.
//
// `derive_struct_move_only` is pinned here too: it is what separates move-only
// from drop, so a struct type built by hand must have it called before anything
// asks `noncopyable()` — the predicate asserts on that rather than defaulting to
// the unsound answer.
//
// Struct behavior with *synthesized* destructors (which require the semantic
// pass) is exercised end-to-end by the build_delete_desc cross-check assertion
// across the e2e suite.

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
        Type* e = types.enum_type("E"_sv, nullptr);
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

        Type* has_ref = types.struct_type("HasRef"_sv, nullptr);
        auto* rf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        rf[0] = FieldInfo{ "r"_sv, types.ref_type(types.i32_type()),
                           /*is_pub=*/true, /*index=*/0, /*slot_offset=*/0, /*slot_count=*/2 };
        has_ref->struct_info.fields = Span<FieldInfo>(rf, 1);
        has_ref->struct_info.when_clauses = Span<WhenClauseInfo>();
        has_ref->struct_info.destructors = Span<DestructorInfo>();
        derive_struct_move_only(has_ref->struct_info);

        CHECK(has_ref->is_copy());        // a `ref` is a borrow, not an owner → copyable
        CHECK(has_ref->needs_drop());     // ... but the ref field must be released
        CHECK(has_ref->needs_retain());   // ... and re-borrowed on copy
        CHECK_FALSE(has_ref->is_trivial());

        // A struct of only plain fields stays trivial.
        Type* plain = types.struct_type("Plain"_sv, nullptr);
        auto* pf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo) * 2, alignof(FieldInfo)));
        pf[0] = FieldInfo{ "x"_sv, types.i32_type(), true, 0, 0, 1 };
        pf[1] = FieldInfo{ "y"_sv, types.f64_type(), true, 1, 1, 2 };
        plain->struct_info.fields = Span<FieldInfo>(pf, 2);
        plain->struct_info.when_clauses = Span<WhenClauseInfo>();
        plain->struct_info.destructors = Span<DestructorInfo>();
        derive_struct_move_only(plain->struct_info);
        derive_struct_move_only(plain->struct_info);

        CHECK(plain->is_copy());
        CHECK_FALSE(plain->needs_drop());
        CHECK(plain->is_trivial());
    }

    // ---- derive_struct_move_only ------------------------------------------
    //
    // Move-only is a property of what a struct HOLDS, not of whether it has a
    // destructor. These pin both directions of the split that used to be one bit.

    // Helper: build a struct with the given fields and destructors, derived.
    static Type* make_struct(BumpAllocator& allocator, TypeCache& types, const char* name,
                             Span<FieldInfo> fields, Span<DestructorInfo> dtors) {
        Type* t = types.struct_type(StringView(name, (u32)strlen(name)), nullptr);
        t->struct_info.fields = fields;
        t->struct_info.when_clauses = Span<WhenClauseInfo>();
        t->struct_info.destructors = dtors;
        derive_struct_move_only(t->struct_info);
        return t;
    }

    static Span<FieldInfo> one_field(BumpAllocator& allocator, const char* name, Type* type) {
        auto* f = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        f[0] = FieldInfo{ StringView(name, (u32)strlen(name)), type, true, 0, 0, 2 };
        return Span<FieldInfo>(f, 1);
    }

    TEST_CASE("move-only is decided by what a struct holds") {
        BumpAllocator allocator(8192);
        TypeCache types(allocator);
        Span<DestructorInfo> no_dtor = Span<DestructorInfo>();

        // Reference-counted members are copyable: their drop has an exact
        // inverse, so a second owner can simply acquire its own count. A
        // `string` field being treated as move-only is what broke reading a
        // plain data record out of a `List`.
        CHECK(make_struct(allocator, types, "HasString",
                          one_field(allocator, "s", types.string_type()), no_dtor)->is_copy());
        CHECK(make_struct(allocator, types, "HasRef",
                          one_field(allocator, "r", types.ref_type(types.i32_type())),
                          no_dtor)->is_copy());

        // Owning members have no inverse — duplicating would hand two owners the
        // same resource — so holding one is exactly what makes a struct move-only.
        Type* uniq_holder = make_struct(allocator, types, "HasUniq",
            one_field(allocator, "u", types.uniq_type(types.i32_type())), no_dtor);
        CHECK_FALSE(uniq_holder->is_copy());
        CHECK_FALSE(make_struct(allocator, types, "HasList",
            one_field(allocator, "xs", types.list_type(types.i32_type())), no_dtor)->is_copy());

        // ... and it propagates through embedding.
        CHECK_FALSE(make_struct(allocator, types, "Outer2",
            one_field(allocator, "inner", uniq_holder), no_dtor)->is_copy());
    }

    TEST_CASE("only a USER-WRITTEN default destructor implies move-only") {
        BumpAllocator allocator(8192);
        TypeCache types(allocator);

        auto make_dtor = [&](Decl* decl) {
            auto* d = reinterpret_cast<DestructorInfo*>(
                allocator.alloc_bytes(sizeof(DestructorInfo), alignof(DestructorInfo)));
            d[0] = DestructorInfo{};
            d[0].decl = decl;
            return Span<DestructorInfo>(d, 1);
        };

        // A synthetic destructor (decl == null) only releases what the fields
        // hold, so it says nothing the fields do not already say. Reading it as
        // "move-only" is the conflation this derivation removes.
        CHECK(make_struct(allocator, types, "SyntheticDtor",
                          one_field(allocator, "s", types.string_type()),
                          make_dtor(nullptr))->is_copy());

        // A user-written one has a body with arbitrary effects, and running it
        // twice for one logical value is precisely what move-only prevents.
        auto* body = reinterpret_cast<Decl*>(allocator.alloc_bytes(sizeof(void*), alignof(void*)));
        CHECK_FALSE(make_struct(allocator, types, "UserDtor",
                                one_field(allocator, "n", types.i32_type()),
                                make_dtor(body))->is_copy());
    }

    // ---- compute_retain_plan: the mirror of compute_drop_plan --------------
    //
    // The invariant these pin: for every type, retain and drop are either both
    // trivial, or exact inverses, or the type is move-only (retain None, drop
    // destructive). No type may have a non-trivial drop, no retain, and still be
    // copyable — that is the shape that leaks or double-frees.

    TEST_CASE("retain plan: trivial types acquire nothing") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* trivial[] = { types.i32_type(), types.i64_type(), types.bool_type(),
                            types.f64_type(), types.weak_type(types.i32_type()) };
        for (Type* t : trivial) {
            CHECK(compute_retain_plan(t).kind == RetainKind::None);
            CHECK(compute_drop_plan(t).kind == DropKind::None);
        }
        CHECK(compute_retain_plan(nullptr).kind == RetainKind::None);
    }

    TEST_CASE("retain plan: string and ref invert their drops exactly") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);

        Type* s = types.string_type();
        CHECK(compute_retain_plan(s).kind == RetainKind::StrRetain);
        CHECK(compute_drop_plan(s).kind == DropKind::StrRelease);
        CHECK(s->is_copy());   // copyable *because* the drop has an inverse

        Type* r = types.ref_type(types.i32_type());
        CHECK(compute_retain_plan(r).kind == RetainKind::RefInc);
        CHECK(compute_drop_plan(r).kind == DropKind::RefDec);
        CHECK(r->is_copy());
    }

    TEST_CASE("retain plan: move-only kinds acquire nothing") {
        // They are moved, never implicitly duplicated; `.copy()` is a separate,
        // explicit deep copy. A retain here would be meaningless — there is no
        // count to bump, only a buffer to duplicate.
        BumpAllocator allocator(4096);
        TypeCache types(allocator);
        Type* move_only[] = {
            types.uniq_type(types.i32_type()),
            types.list_type(types.i32_type()),
            types.map_type(types.string_type(), types.i32_type()),
        };
        for (Type* t : move_only) {
            CHECK_FALSE(t->is_copy());
            CHECK(compute_retain_plan(t).kind == RetainKind::None);
        }
    }

    TEST_CASE("retain plan: a copyable struct walks fields; move-only does not") {
        BumpAllocator allocator(4096);
        TypeCache types(allocator);

        // A copyable struct holding a `string` retains by walking its fields.
        // This is the shape that leaks today: the walk has no emitter yet.
        Type* boxed = types.struct_type("Boxed"_sv, nullptr);
        auto* bf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        bf[0] = FieldInfo{ "s"_sv, types.string_type(), true, 0, 0, 2 };
        boxed->struct_info.fields = Span<FieldInfo>(bf, 1);
        boxed->struct_info.when_clauses = Span<WhenClauseInfo>();
        boxed->struct_info.destructors = Span<DestructorInfo>();
        derive_struct_move_only(boxed->struct_info);

        CHECK(boxed->is_copy());
        RetainPlan bp = compute_retain_plan(boxed);
        CHECK(bp.kind == RetainKind::WalkFields);
        CHECK(bp.struct_type == boxed);

        // Nesting: a struct whose field is *that* struct also walks (the emitter
        // recurses by consulting the plan per field).
        Type* outer = types.struct_type("Outer"_sv, nullptr);
        auto* of = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        of[0] = FieldInfo{ "inner"_sv, boxed, true, 0, 0, 2 };
        outer->struct_info.fields = Span<FieldInfo>(of, 1);
        outer->struct_info.when_clauses = Span<WhenClauseInfo>();
        outer->struct_info.destructors = Span<DestructorInfo>();
        derive_struct_move_only(outer->struct_info);
        CHECK(compute_retain_plan(outer).kind == RetainKind::WalkFields);

        // A struct of plain fields retains nothing.
        Type* plain = types.struct_type("PlainR"_sv, nullptr);
        auto* pf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        pf[0] = FieldInfo{ "x"_sv, types.i32_type(), true, 0, 0, 1 };
        plain->struct_info.fields = Span<FieldInfo>(pf, 1);
        plain->struct_info.when_clauses = Span<WhenClauseInfo>();
        plain->struct_info.destructors = Span<DestructorInfo>();
        derive_struct_move_only(plain->struct_info);
        CHECK(compute_retain_plan(plain).kind == RetainKind::None);

        // A struct made move-only by a USER-WRITTEN destructor retains nothing
        // even though a field would: it is moved, not duplicated. The `decl` is
        // what carries that distinction — a synthetic destructor (decl == null)
        // only releases what the fields hold and so does not imply move-only.
        Type* owned = types.struct_type("OwnedR"_sv, nullptr);
        auto* wf = reinterpret_cast<FieldInfo*>(
            allocator.alloc_bytes(sizeof(FieldInfo), alignof(FieldInfo)));
        wf[0] = FieldInfo{ "s"_sv, types.string_type(), true, 0, 0, 2 };
        owned->struct_info.fields = Span<FieldInfo>(wf, 1);
        owned->struct_info.when_clauses = Span<WhenClauseInfo>();
        auto* dt = reinterpret_cast<DestructorInfo*>(
            allocator.alloc_bytes(sizeof(DestructorInfo), alignof(DestructorInfo)));
        dt[0] = DestructorInfo{};
        dt[0].decl = reinterpret_cast<Decl*>(
            allocator.alloc_bytes(sizeof(void*), alignof(void*)));  // stands in for a body
        owned->struct_info.destructors = Span<DestructorInfo>(dt, 1);
        derive_struct_move_only(owned->struct_info);
        CHECK_FALSE(owned->is_copy());
        CHECK(compute_retain_plan(owned).kind == RetainKind::None);
    }
}

#include "roxy/compiler/trait_system.hpp"

#include <cstring>

namespace rx {

// ===== Builtin trait registration (Pass 1) =====

Type* TraitSystem::register_builtin_trait(
        StringView name, StringView method_name,
        Span<Type*> method_param_types, Type* return_type,
        Span<TypeKind> primitive_kinds, bool register_trait_on_primitives) {
    Type* trait_type = m_types.trait_type(name, nullptr);
    m_type_env.register_trait_type(name, trait_type);

    TraitMethodInfo tmi;
    tmi.name = method_name;
    tmi.param_types = method_param_types;
    tmi.return_type = return_type;
    tmi.decl = nullptr;
    tmi.has_default = false;
    trait_type->trait_info.methods =
        Span<TraitMethodInfo>(m_allocator.emplace<TraitMethodInfo>(tmi), 1);

    for (TypeKind tk : primitive_kinds) {
        MethodInfo mi;
        mi.name = method_name;
        mi.param_types = method_param_types;
        mi.return_type = return_type;
        mi.decl = nullptr;
        m_types.register_primitive_method(tk, mi);
        if (register_trait_on_primitives) {
            m_types.register_primitive_trait(tk, trait_type);
        }
    }
    return trait_type;
}

Type* TraitSystem::register_builtin_index_trait(StringView name, StringView method_name,
                                                     bool is_mut) {
    Type* trait_type = m_types.trait_type(name, nullptr);
    m_type_env.register_trait_type(name, trait_type);

    // Two type params: <Idx, Output>. Output appears in the method's return type
    // (index) or value parameter (index_mut), since Roxy has no associated types.
    Vector<TypeParam> tparams;
    TypeParam idx_param; idx_param.name = StringView("Idx", 3);
    idx_param.loc = SourceLocation{}; idx_param.bounds = Span<TypeExpr*>();
    TypeParam out_param; out_param.name = StringView("Output", 6);
    out_param.loc = SourceLocation{}; out_param.bounds = Span<TypeExpr*>();
    tparams.push_back(idx_param);
    tparams.push_back(out_param);
    trait_type->trait_info.type_params = m_allocator.alloc_span(tparams);

    Type* idx_tp = m_types.type_param(StringView("Idx", 3), 0);
    Type* out_tp = m_types.type_param(StringView("Output", 6), 1);

    Vector<Type*> method_params;
    method_params.push_back(idx_tp);
    Type* return_type;
    if (is_mut) {
        method_params.push_back(out_tp);       // index_mut(idx: Idx, val: Output)
        return_type = m_types.void_type();
    } else {
        return_type = out_tp;                  // index(idx: Idx): Output
    }

    TraitMethodInfo tmi;
    tmi.name = method_name;
    tmi.param_types = m_allocator.alloc_span(method_params);
    tmi.return_type = return_type;
    tmi.decl = nullptr;
    tmi.has_default = false;
    trait_type->trait_info.methods =
        Span<TraitMethodInfo>(m_allocator.emplace<TraitMethodInfo>(tmi), 1);
    return trait_type;
}

void TraitSystem::register_builtin_traits() {
    if (!m_type_env.printable_type()) {
        TypeKind prim_kinds[] = {
            TypeKind::Bool, TypeKind::I32, TypeKind::I64, TypeKind::U32, TypeKind::U64,
            TypeKind::F32, TypeKind::F64, TypeKind::String
        };
        Type* printable_type = register_builtin_trait(
            StringView("Printable", 9), StringView("to_string", 9),
            Span<Type*>(nullptr, 0), m_types.string_type(),
            Span<TypeKind>(prim_kinds, 8), /*register_trait_on_primitives=*/true);
        m_type_env.set_printable_type(printable_type);
    }

    if (!m_type_env.hash_type()) {
        TypeKind hashable_kinds[] = {
            TypeKind::Bool,
            TypeKind::I8, TypeKind::I16, TypeKind::I32, TypeKind::I64,
            TypeKind::U8, TypeKind::U16, TypeKind::U32, TypeKind::U64,
            TypeKind::F32, TypeKind::F64,
            TypeKind::String
        };
        Type* hash_trait_type = register_builtin_trait(
            StringView("Hash", 4), StringView("hash", 4),
            Span<Type*>(nullptr, 0), m_types.u64_type(),
            Span<TypeKind>(hashable_kinds, 12), /*register_trait_on_primitives=*/true);
        m_type_env.set_hash_type(hash_trait_type);
    }

    // Builtin `Eq` trait used by Map's struct-key dispatch — `Map<K, V>` uses
    // custom hash/eq runtime dispatch only when K explicitly implements both
    // `Hash` and `Eq` (just defining `hash()` / `eq()` methods isn't enough).
    // We declare `eq(other: Self): bool` as the required trait method so user
    // `for Eq` impls match against it, but we don't register `eq` as a method
    // on primitive types — that would shadow the native operator-dispatch
    // path for `==` on primitives.
    //
    // Tests / user code may also write `trait Eq;` to declare the trait for
    // operator overloading — collect_trait_declaration (the trait-decl handler) merges that with
    // this builtin instead of erroring on duplicate.
    if (!m_type_env.eq_type()) {
        // Required method: fun Eq.eq(other: Self): bool. Eq is intentionally not
        // registered on primitives (see comment above), so primitive_kinds is
        // empty and register_trait_on_primitives is irrelevant.
        Span<Type*> eq_params(m_allocator.emplace<Type*>(m_types.self_type()), 1);
        Type* eq_trait_type = register_builtin_trait(
            StringView("Eq", 2), StringView("eq", 2),
            eq_params, m_types.bool_type(),
            Span<TypeKind>(), /*register_trait_on_primitives=*/false);
        m_type_env.set_eq_type(eq_trait_type);
    }

    if (!m_type_env.exception_type()) {
        // `message` is installed on ExceptionRef as a primitive method, but the
        // Exception trait itself is not registered on it (catch-all handles only
        // message()), so register_trait_on_primitives is false.
        TypeKind exc_kinds[] = { TypeKind::ExceptionRef };
        Type* exception_trait_type = register_builtin_trait(
            StringView("Exception", 9), StringView("message", 7),
            Span<Type*>(nullptr, 0), m_types.string_type(),
            Span<TypeKind>(exc_kinds, 1), /*register_trait_on_primitives=*/false);
        m_type_env.set_exception_type(exception_trait_type);
    }

    // Builtin subscript-operator traits. `a[i]` rewrites to `a.index(i)` and
    // `a[i] = v` to `a.index_mut(i, v)`; the dispatch is structural (any matching
    // method), but these traits let user types formally opt in with
    // `for Index<Idx, Output>` / `for IndexMut<Idx, Output>`. Built with two type
    // params because Roxy has no associated types for the element (output) type.
    // Registered with decl == nullptr so a user `trait Index<Idx, Output>;` merges
    // rather than colliding (see collect_trait_declaration).
    if (!m_type_env.trait_type_by_name(StringView("Index", 5))) {
        register_builtin_index_trait(StringView("Index", 5), StringView("index", 5),
                                     /*is_mut=*/false);
    }
    if (!m_type_env.trait_type_by_name(StringView("IndexMut", 8))) {
        register_builtin_index_trait(StringView("IndexMut", 8), StringView("index_mut", 9),
                                     /*is_mut=*/true);
    }
}

// ===== Primitive Operator Registration =====

void TraitSystem::register_primitive_operator_methods() {
    // Guard: only register once (TypeEnv persists across modules)
    // Use I32's "add" method as a sentinel — if it's already registered, skip.
    if (m_types.lookup_primitive_method(TypeKind::I32, StringView("add", 3))) return;

    // Helper: allocate a Span<Type*> with one element from the bump allocator
    auto make_param_span = [this](Type* param) -> Span<Type*> {
        Type** data = reinterpret_cast<Type**>(
            m_allocator.alloc_bytes(sizeof(Type*), alignof(Type*)));
        data[0] = param;
        return Span<Type*>(data, 1);
    };

    Span<Type*> no_params(nullptr, 0);

    // Helper: register a batch of operator methods for a primitive type
    auto register_ops = [this](TypeKind kind, const char* const* op_names, u32 count,
                               Span<Type*> param_types, Type* return_type) {
        for (u32 i = 0; i < count; i++) {
            MethodInfo method_info;
            method_info.name = StringView(op_names[i], static_cast<u32>(strlen(op_names[i])));
            method_info.param_types = param_types;
            method_info.return_type = return_type;
            method_info.decl = nullptr;
            m_types.register_primitive_method(kind, method_info);
        }
    };

    // Register for integer types (I32, I64) and the native unsigned types U32/U64.
    // The IR builder selects the unsigned IR ops (DivU/ModU/LtU../UShr) for
    // div/mod/ordered-compare/shr where signedness matters; add/sub/mul/shl/bitwise
    // reuse the shared ops. `return_type = entry.type` makes `uN op uN -> uN`.
    // U32 values are kept canonically zero-extended (lowering's TRUNC_U 32 hook)
    // so the 64-bit unsigned ops are correct and results wrap at 32 bits.
    struct { TypeKind kind; Type* type; } int_types[] = {
        { TypeKind::I32, m_types.i32_type() },
        { TypeKind::I64, m_types.i64_type() },
        { TypeKind::U32, m_types.u32_type() },
        { TypeKind::U64, m_types.u64_type() },
    };
    for (auto& entry : int_types) {
        Span<Type*> self_param = make_param_span(entry.type);

        const char* arith_ops[] = { "add", "sub", "mul", "div", "mod" };
        register_ops(entry.kind, arith_ops, 5, self_param, entry.type);

        const char* bit_ops[] = { "bit_and", "bit_or", "bit_xor", "shl", "shr" };
        register_ops(entry.kind, bit_ops, 5, self_param, entry.type);

        const char* cmp_ops[] = { "eq", "ne", "lt", "le", "gt", "ge" };
        register_ops(entry.kind, cmp_ops, 6, self_param, m_types.bool_type());

        const char* unary_ops[] = { "neg", "bit_not" };
        register_ops(entry.kind, unary_ops, 2, no_params, entry.type);

        const char* compound_ops[] = {
            "add_assign", "sub_assign", "mul_assign", "div_assign", "mod_assign",
            "bit_and_assign", "bit_or_assign", "bit_xor_assign", "shl_assign", "shr_assign"
        };
        register_ops(entry.kind, compound_ops, 10, self_param, m_types.void_type());
    }

    // Register for float types (F32, F64)
    struct { TypeKind kind; Type* type; } float_types[] = {
        { TypeKind::F32, m_types.f32_type() },
        { TypeKind::F64, m_types.f64_type() },
    };
    for (auto& entry : float_types) {
        Span<Type*> self_param = make_param_span(entry.type);

        const char* arith_ops[] = { "add", "sub", "mul", "div" };
        register_ops(entry.kind, arith_ops, 4, self_param, entry.type);

        const char* cmp_ops[] = { "eq", "ne", "lt", "le", "gt", "ge" };
        register_ops(entry.kind, cmp_ops, 6, self_param, m_types.bool_type());

        const char* unary_ops[] = { "neg" };
        register_ops(entry.kind, unary_ops, 1, no_params, entry.type);

        const char* compound_ops[] = { "add_assign", "sub_assign", "mul_assign", "div_assign" };
        register_ops(entry.kind, compound_ops, 4, self_param, m_types.void_type());
    }

    // Equality for the remaining narrow integer kinds (i8/i16/u8/u16):
    // same-type equality on canonically stored values is a bit comparison,
    // so the signed EqI/NeI lowering is correct regardless of signedness.
    // Ordered comparisons, arithmetic, and bitwise ops are deliberately NOT
    // registered: these promote to i32 for arithmetic (handled before operator
    // resolution). (u32 and u64 have native arithmetic — the block above.)
    {
        struct { TypeKind kind; Type* type; } equality_only_int_types[] = {
            { TypeKind::I8,  m_types.i8_type()  },
            { TypeKind::I16, m_types.i16_type() },
            { TypeKind::U8,  m_types.u8_type()  },
            { TypeKind::U16, m_types.u16_type() },
        };
        const char* eq_ops[] = { "eq", "ne" };
        for (auto& entry : equality_only_int_types) {
            Span<Type*> self_param = make_param_span(entry.type);
            register_ops(entry.kind, eq_ops, 2, self_param, m_types.bool_type());
        }
    }

    // Register for bool: eq, ne → bool
    {
        Span<Type*> bool_param = make_param_span(m_types.bool_type());
        const char* bool_ops[] = { "eq", "ne" };
        register_ops(TypeKind::Bool, bool_ops, 2, bool_param, m_types.bool_type());
    }

    // Register for string: eq, ne → bool
    {
        Span<Type*> string_param = make_param_span(m_types.string_type());
        const char* string_ops[] = { "eq", "ne" };
        register_ops(TypeKind::String, string_ops, 2, string_param, m_types.bool_type());
    }
}

// ===== Trait declaration collection (Pass 1) =====

void TraitSystem::collect_trait_declaration(Decl* decl) {
    StringView name = decl->trait_decl.name;

    // Allow user redeclaration of builtin traits (Hash, Eq, etc.):
    // a builtin trait was registered with `decl == nullptr` at this
    // pass's start; if the user writes `trait Eq;`, attach their decl
    // to the existing trait type rather than erroring on duplicate.
    // The user's trait method declarations (later in Pass 2) populate
    // the trait's methods on the existing type.
    Type* existing_trait = m_type_env.trait_type_by_name(name);
    if (existing_trait != nullptr && existing_trait->trait_info.decl == nullptr) {
        existing_trait->trait_info.decl = decl;
        existing_trait->trait_info.type_params = decl->trait_decl.type_params;
        m_symbols.define(SymbolKind::Trait, name, existing_trait, decl->loc, decl);
        return;
    }

    // Check for duplicate type/trait names
    if (m_type_env.named_type_by_name(name) != nullptr ||
        existing_trait != nullptr) {
        m_reporter.error_fmt(decl->loc, "duplicate type declaration '{}'", name);
        return;
    }

    // Create the trait type
    Type* type = m_types.trait_type(name, decl);
    type->trait_info.type_params = decl->trait_decl.type_params;
    m_type_env.register_trait_type(name, type);

    // Define in global scope
    m_symbols.define(SymbolKind::Trait, name, type, decl->loc, decl);
}

// ===== Trait member resolution (Pass 2) =====

void TraitSystem::resolve_trait_parent(Decl* decl) {
    TraitDecl& trait_decl = decl->trait_decl;
    Type* trait_type = m_type_env.trait_type_by_name(trait_decl.name);

    // Resolve parent trait
    if (!trait_decl.parent_name.empty()) {
        Type* parent_trait = m_type_env.trait_type_by_name(trait_decl.parent_name);
        if (!parent_trait) {
            m_reporter.error_fmt(decl->loc, "unknown parent trait '{}'", trait_decl.parent_name);
        } else if (parent_trait == trait_type) {
            m_reporter.error_fmt(decl->loc, "trait '{}' cannot inherit from itself", trait_decl.name);
        } else {
            trait_type->trait_info.parent = parent_trait;
        }
    }
}

Type* TraitSystem::resolve_trait_method_type_expr(TypeExpr* type_expr,
                                                 const TraitTypeInfo& trait_info) {
    if (!type_expr) {
        return m_types.error_type();
    }
    if (type_expr->name == "Self") {
        return m_types.self_type();
    }
    // Check if this is a trait type param
    for (u32 i = 0; i < trait_info.type_params.size(); i++) {
        if (type_expr->name == trait_info.type_params[i].name) {
            return m_types.type_param(trait_info.type_params[i].name, i);
        }
    }
    return m_context.resolve_type_expr(type_expr);
}

void TraitSystem::register_trait_method_signature(Decl* decl, Type* trait_type) {
    MethodDecl& method_decl = decl->method_decl;

    // Check for duplicate method names in this trait. Builtin traits (Hash,
    // Eq, etc.) pre-register their required methods with `decl == nullptr`;
    // if the user redeclares the same method (matching name + arity), allow
    // it as an idempotent re-declaration so user `trait Eq; fun Eq.eq(other:
    // Self): bool;` doesn't conflict with the builtin Eq registration.
    TraitTypeInfo& trait_type_info = trait_type->trait_info;
    for (const auto& trait_method : trait_type_info.methods) {
        if (trait_method.name == method_decl.name) {
            bool is_builtin_redecl = trait_method.decl == nullptr &&
                trait_method.param_types.size() == method_decl.params.size();
            if (is_builtin_redecl) {
                // The builtin shape stays; ignore the redeclaration silently.
                return;
            }
            m_reporter.error_fmt(decl->loc, "duplicate trait method '{}' in trait '{}'",
                     method_decl.name, trait_type_info.name);
            return;
        }
    }

    // Resolve parameter types - use TypeKind::Self for Self, TypeParam for trait type params
    Vector<Type*> param_types;
    for (const auto& param : method_decl.params) {
        param_types.push_back(resolve_trait_method_type_expr(param.type, trait_type_info));
    }

    // Resolve return type (TypeKind::Self for Self, TypeParam for trait type params)
    Type* return_type = method_decl.return_type
        ? resolve_trait_method_type_expr(method_decl.return_type, trait_type_info)
        : m_types.void_type();

    // Create trait method info
    TraitMethodInfo trait_method_info;
    trait_method_info.name = method_decl.name;
    trait_method_info.param_types = m_allocator.alloc_span(param_types);
    trait_method_info.return_type = return_type;
    trait_method_info.decl = decl;
    trait_method_info.has_default = (method_decl.body != nullptr);

    // Add to trait's method list
    Vector<TraitMethodInfo> methods;
    for (const auto& method : trait_type_info.methods) {
        methods.push_back(method);
    }
    methods.push_back(trait_method_info);

    trait_type_info.methods = m_allocator.alloc_span(methods);
}

void TraitSystem::resolve_trait_impl_member(Decl* decl) {
    MethodDecl& method_decl = decl->method_decl;

    Type* struct_type_lookup = m_type_env.named_type_by_name(method_decl.struct_name);
    if (!struct_type_lookup) {
        m_reporter.error_fmt(decl->loc, "method for unknown type '{}'", method_decl.struct_name);
    } else if (struct_type_lookup->kind != TypeKind::Struct) {
        m_reporter.error_fmt(decl->loc, "'{}' is not a struct type", method_decl.struct_name);
    } else {
        Type* impl_trait = m_type_env.trait_type_by_name(method_decl.trait_name);
        if (!impl_trait) {
            m_reporter.error_fmt(decl->loc, "unknown trait '{}'", method_decl.trait_name);
        } else {
            Type* trait_type = impl_trait;
            TraitTypeInfo& trait_type_info = trait_type->trait_info;

            // Resolve the `for Trait<Args>` type args of this impl.
            Span<Type*> resolved_trait_type_args;
            if (!resolve_trait_impl_type_args(method_decl, trait_type_info,
                                              struct_type_lookup, decl->loc,
                                              resolved_trait_type_args)) {
                return;
            }

            m_pending_trait_impls.push_back({decl, struct_type_lookup, trait_type, resolved_trait_type_args});
        }
    }
}

// ===== Trait impl validation (Pass 2) =====

bool TraitSystem::resolve_trait_impl_type_args(
        MethodDecl& method_decl, const TraitTypeInfo& trait_info,
        Type* struct_type, SourceLocation loc, Span<Type*>& out) {
    out = Span<Type*>();
    if (method_decl.trait_type_args.size() > 0) {
        if (trait_info.type_params.size() == 0) {
            m_reporter.error_fmt(loc, "trait '{}' does not take type arguments", method_decl.trait_name);
            return false;
        }
        if (method_decl.trait_type_args.size() != trait_info.type_params.size()) {
            m_reporter.error_fmt(loc, "trait '{}' expects {} type argument(s), got {}",
                     method_decl.trait_name, trait_info.type_params.size(),
                     method_decl.trait_type_args.size());
            return false;
        }
        Vector<Type*> args;
        for (auto* type_arg : method_decl.trait_type_args) {
            args.push_back(m_context.resolve_type_expr(type_arg));
        }
        out = m_allocator.alloc_span(args);
    } else if (trait_info.type_params.size() > 0) {
        // Default all type params to Self (the implementing struct type).
        // Enables `for Add` as shorthand for `for Add<Vec2>` on struct Vec2.
        Vector<Type*> args;
        for (u32 i = 0; i < trait_info.type_params.size(); i++) {
            args.push_back(struct_type);
        }
        out = m_allocator.alloc_span(args);
    }
    return true;
}

Vector<TraitSystem::TraitImplGroup> TraitSystem::group_pending_trait_impls() {
    // Group pending impls by (struct, trait, type_args). A simple list of
    // groups suffices since we don't expect many.
    Vector<TraitImplGroup> groups;
    for (auto& pending : m_pending_trait_impls) {
        TraitImplGroup* group = nullptr;
        for (auto& g : groups) {
            if (g.struct_type != pending.struct_type || g.trait_type != pending.trait_type) continue;
            // Match on type args element-wise too.
            bool args_match = g.trait_type_args.size() == pending.trait_type_args.size();
            if (args_match) {
                for (u32 i = 0; i < g.trait_type_args.size(); i++) {
                    if (g.trait_type_args[i] != pending.trait_type_args[i]) { args_match = false; break; }
                }
            }
            if (args_match) { group = &g; break; }
        }
        if (!group) {
            groups.push_back({pending.struct_type, pending.trait_type, pending.trait_type_args, {}});
            group = &groups.back();
        }
        group->impl_decls.push_back(pending.decl);
    }
    return groups;
}

bool TraitSystem::check_parent_trait_satisfied(const TraitImplGroup& group,
                                                    const Vector<TraitImplGroup>& all_groups) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    if (!trait_type_info.parent) return true;

    StructTypeInfo& struct_type_info = group.struct_type->struct_info;
    for (const auto& impl : struct_type_info.implemented_traits) {
        if (impl.trait == trait_type_info.parent) return true;
    }
    // Also satisfied if the parent trait is implemented in this same batch.
    for (const auto& other : all_groups) {
        if (other.struct_type == group.struct_type && other.trait_type == trait_type_info.parent) {
            return true;
        }
    }
    m_reporter.error_fmt(group.impl_decls[0]->loc,
             "trait '{}' requires parent trait '{}' to be implemented for '{}'",
             trait_type_info.name, trait_type_info.parent->trait_info.name, struct_type_info.name);
    return false;
}

void TraitSystem::validate_and_register_impl_method(const TraitImplGroup& group, Decl* decl,
                                                         Vector<bool>& implemented) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    StructTypeInfo& struct_type_info = group.struct_type->struct_info;
    MethodDecl& method_decl = decl->method_decl;

    for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
        const TraitMethodInfo& trait_method = trait_type_info.methods[i];
        if (trait_method.name != method_decl.name) continue;
        implemented[i] = true;

        // Validate parameter count matches.
        if (method_decl.params.size() != trait_method.param_types.size()) {
            m_reporter.error_fmt(decl->loc, "method '{}' parameter count mismatch with trait '{}'",
                     method_decl.name, trait_type_info.name);
        }

        // Resolve the impl's param/return types.
        Vector<Type*> param_types;
        for (const auto& param : method_decl.params) {
            param_types.push_back(m_context.resolve_type_expr(param.type));
        }
        Type* return_type = method_decl.return_type ? m_context.resolve_type_expr(method_decl.return_type) : m_types.void_type();

        // Validate parameter types match the trait signature (Self / trait
        // type-params concretized against this impl).
        if (method_decl.params.size() == trait_method.param_types.size()) {
            for (u32 p = 0; p < param_types.size(); p++) {
                if (param_types[p]->is_error()) continue;
                Type* expected = concretize_trait_type(trait_method.param_types[p],
                                                       group.struct_type, group.trait_type_args);
                if (param_types[p] != expected) {
                    auto got_str = m_checker.type_string(param_types[p]);
                    auto exp_str = m_checker.type_string(expected);
                    m_reporter.error_fmt(decl->loc,
                             "method '{}' parameter {} has type '{}' but trait '{}' expects '{}'",
                             method_decl.name, p + 1, got_str.data(), trait_type_info.name, exp_str.data());
                }
            }
        }

        // Validate return type matches the trait signature.
        if (!return_type->is_error()) {
            Type* expected_ret = concretize_trait_type(trait_method.return_type,
                                                       group.struct_type, group.trait_type_args);
            if (return_type != expected_ret) {
                auto got_str = m_checker.type_string(return_type);
                auto exp_str = m_checker.type_string(expected_ret);
                m_reporter.error_fmt(decl->loc,
                         "method '{}' return type is '{}' but trait '{}' expects '{}'",
                         method_decl.name, got_str.data(), trait_type_info.name, exp_str.data());
            }
        }

        // Register as a regular method on the struct, unless one already exists.
        bool is_duplicate = false;
        for (const auto& method : struct_type_info.methods) {
            if (method.name == method_decl.name) { is_duplicate = true; break; }
        }
        if (!is_duplicate) {
            MethodInfo method_info;
            method_info.name = method_decl.name;
            method_info.param_types = m_allocator.alloc_span(param_types);
            method_info.return_type = return_type;
            method_info.decl = decl;
            append_method(m_allocator, struct_type_info, method_info);
        }
        return;  // matched a trait method
    }

    m_reporter.error_fmt(decl->loc, "method '{}' is not defined in trait '{}'",
             method_decl.name, trait_type_info.name);
}

void TraitSystem::finalize_trait_impl(const TraitImplGroup& group, const Vector<bool>& implemented) {
    TraitTypeInfo& trait_type_info = group.trait_type->trait_info;
    StructTypeInfo& struct_type_info = group.struct_type->struct_info;

    // Missing required methods; inject defaults where the trait provides one.
    for (u32 i = 0; i < trait_type_info.methods.size(); i++) {
        if (implemented[i]) continue;
        if (trait_type_info.methods[i].has_default) {
            inject_default_method(group.struct_type, group.trait_type,
                                  trait_type_info.methods[i], group.trait_type_args);
        } else {
            m_reporter.error_fmt(group.impl_decls[0]->loc,
                     "trait '{}' requires method '{}' which is not implemented for '{}'",
                     trait_type_info.name, trait_type_info.methods[i].name,
                     struct_type_info.name);
        }
    }

    // Append this trait to the struct's implemented_traits list.
    Vector<TraitImplRecord> trait_records;
    for (const auto& impl : struct_type_info.implemented_traits) {
        trait_records.push_back(impl);
    }
    trait_records.push_back(TraitImplRecord{group.trait_type, group.trait_type_args});
    struct_type_info.implemented_traits = m_allocator.alloc_span(trait_records);
}

void TraitSystem::validate_trait_implementations() {
    Vector<TraitImplGroup> groups = group_pending_trait_impls();
    for (auto& group : groups) {
        if (!check_parent_trait_satisfied(group, groups)) continue;

        Vector<bool> implemented(group.trait_type->trait_info.methods.size(), false);
        for (Decl* decl : group.impl_decls) {
            validate_and_register_impl_method(group, decl, implemented);
        }
        finalize_trait_impl(group, implemented);
    }
}

Type* TraitSystem::concretize_trait_type(Type* abstract_type, Type* struct_type, Span<Type*> trait_type_args) {
    if (abstract_type->is_self()) return struct_type;
    if (abstract_type->is_type_param() && trait_type_args.size() > 0) {
        u32 idx = abstract_type->type_param_info.index;
        if (idx < trait_type_args.size()) return trait_type_args[idx];
    }
    return abstract_type;
}

void TraitSystem::inject_default_method(Type* struct_type, Type* trait_type,
                                              TraitMethodInfo& trait_method_info, Span<Type*> trait_type_args) {
    // Create a synthetic DeclMethod that targets the implementing struct
    // with a cloned body from the trait's default method
    Decl* trait_decl = trait_method_info.decl;
    MethodDecl& trait_md = trait_decl->method_decl;
    TraitTypeInfo& trait_type_info = trait_type->trait_info;

    Decl* synth = m_allocator.emplace<Decl>();
    synth->kind = AstKind::DeclMethod;
    synth->loc = trait_decl->loc;
    synth->method_decl.struct_name = struct_type->struct_info.name;
    synth->method_decl.name = trait_method_info.name;
    synth->method_decl.type_params = Span<TypeParam>();
    synth->method_decl.is_pub = trait_md.is_pub;
    synth->method_decl.is_native = false;
    synth->method_decl.trait_name = trait_type->trait_info.name;
    synth->method_decl.trait_type_args = Span<TypeExpr*>();

    // Build type substitution: always include Self -> struct_type,
    // plus trait type params -> concrete type args for generic traits
    {
        Vector<StringView> param_names;
        Vector<Type*> concrete_types;

        // Always substitute Self -> struct_type
        param_names.push_back(StringView("Self", 4));
        concrete_types.push_back(struct_type);

        // Add trait type params for generic traits
        if (trait_type_info.type_params.size() > 0 && trait_type_args.size() > 0) {
            for (u32 i = 0; i < trait_type_info.type_params.size(); i++) {
                param_names.push_back(trait_type_info.type_params[i].name);
                concrete_types.push_back(trait_type_args[i]);
            }
        }

        TypeSubstitution subst;
        subst.param_names = m_allocator.alloc_span(param_names);
        subst.concrete_types = m_allocator.alloc_span(concrete_types);

        // Clone body with type substitution
        synth->method_decl.body = m_type_env.generics().clone_stmt(trait_md.body, subst);

        // Clone params with type-expr substitution
        Vector<Param> cloned_params;
        for (const auto& param : trait_md.params) {
            Param p = param;
            p.type = m_type_env.generics().substitute_type_expr(param.type, subst);
            cloned_params.push_back(p);
        }
        synth->method_decl.params = m_allocator.alloc_span(cloned_params);
        synth->method_decl.return_type = m_type_env.generics().substitute_type_expr(trait_md.return_type, subst);
    }

    // Resolve concrete param types (Self -> struct_type, TypeParam -> concrete type)
    StructTypeInfo& struct_type_info = struct_type->struct_info;
    Vector<Type*> param_types;
    for (auto* param_type : trait_method_info.param_types) {
        param_types.push_back(concretize_trait_type(param_type, struct_type, trait_type_args));
    }
    Type* return_type = concretize_trait_type(trait_method_info.return_type, struct_type, trait_type_args);

    // Add MethodInfo to struct's method list
    MethodInfo method_info;
    method_info.name = trait_method_info.name;
    method_info.param_types = m_allocator.alloc_span(param_types);
    method_info.return_type = return_type;
    method_info.decl = synth;

    append_method(m_allocator, struct_type_info, method_info);

    // Add to synthetic decls list for IR builder
    m_synthetic_decls.push_back(synth);
}

}

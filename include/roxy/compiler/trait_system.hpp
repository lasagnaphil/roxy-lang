#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/compiler/error_reporter.hpp"
#include "roxy/compiler/type_checker.hpp"
#include "roxy/compiler/sema_context.hpp"

namespace rx {

// TraitSystem owns the trait machinery the semantic analyzer drives during
// the declaration passes: builtin trait registration (Printable/Hash/Eq/
// Exception/Index/IndexMut and the primitive operator-method tables), user
// trait declaration collection, trait method signature resolution, trait-impl
// grouping/validation, and default-method injection. Collaborators come in
// through the shared SemaContext (same extraction pattern as LifetimeChecker/
// TypeChecker — no back-reference to the analyzer; TypeExpr resolution goes
// through SemaContext::resolve_type_expr).
//
// NOT here: trait *bounds* on generic type parameters and the Phase B
// type-param method lookup (`lookup_type_param_method`, operator dispatch
// through bounds) — those live on GenericCallResolver (see
// generic_call_resolver.hpp).
class TraitSystem {
public:
    TraitSystem(SemaContext& context, Vector<Decl*>& synthetic_decls)
        : m_context(context)
        , m_allocator(context.allocator)
        , m_type_env(context.type_env)
        , m_types(context.types)
        , m_symbols(context.symbols)
        , m_reporter(context.reporter)
        , m_checker(context.checker)
        , m_synthetic_decls(synthetic_decls) {}

    // ===== Pass 1 (type collection) =====

    // Handle a user `trait` declaration: create and register the trait type,
    // or merge with a builtin trait pre-registered with decl == nullptr
    // (user `trait Eq;` attaches to the builtin Eq instead of colliding).
    void collect_trait_declaration(Decl* decl);

    // Register the builtin traits (Printable, Hash, Eq, Exception, and the
    // Index/IndexMut subscript-operator traits), installing their required
    // methods on the matching primitive kinds. Guarded against
    // re-initialization (TypeEnv persists across modules).
    void register_builtin_traits();

    // Register built-in operator trait methods (add/eq/neg/add_assign/...)
    // for primitive types. Guarded against re-initialization.
    void register_primitive_operator_methods();

    // ===== Pass 2 (member resolution) =====

    // Resolve a trait declaration's parent trait (trait inheritance).
    void resolve_trait_parent(Decl* decl);

    // Trait method declaration: fun TraitName.method(...) — registers the
    // method (or merges an idempotent redeclaration of a builtin's method)
    // on the trait type.
    void register_trait_method_signature(Decl* decl, Type* trait_type);

    // Trait implementation member: fun Type.method(...) for Trait<Args> —
    // validates the target struct and trait, resolves the impl's trait type
    // args, and queues the impl for validate_trait_implementations().
    void resolve_trait_impl_member(Decl* decl);

    // Trailing Pass 2 step: group the queued impls by (struct, trait, args),
    // check parent-trait requirements, validate each method against the trait
    // signature, inject defaults for missing methods, and record the
    // implemented trait on the struct.
    void validate_trait_implementations();

private:
    // Register a builtin trait (Printable/Hash/Eq/Exception): create the trait
    // type, register it by name, and attach a single required method. The method
    // (and, when register_trait_on_primitives is set, the trait itself) is also
    // installed on each kind in primitive_kinds. Returns the trait type so the
    // caller can stash it in the matching TypeEnv slot.
    Type* register_builtin_trait(StringView name, StringView method_name,
                                 Span<Type*> method_param_types, Type* return_type,
                                 Span<TypeKind> primitive_kinds,
                                 bool register_trait_on_primitives);

    // Register a builtin generic operator trait for the subscript operator:
    //   Index<Idx, Output>     with  index(idx: Idx): Output
    //   IndexMut<Idx, Output>  with  index_mut(idx: Idx, val: Output)
    // `is_mut` selects index_mut (void return, extra value param) vs index.
    Type* register_builtin_index_trait(StringView name, StringView method_name, bool is_mut);

    // Resolve a trait *method signature* TypeExpr: maps `Self` and the trait's
    // own type-param names to abstract types. (Distinct from concretize_trait_type.)
    Type* resolve_trait_method_type_expr(TypeExpr* type_expr, const TraitTypeInfo& trait_info);

    // Resolve the `for Trait<Args>` type args of a trait impl: validates arity,
    // defaults a bare `for Trait` to all-Self, resolves each arg. Returns false
    // (error already reported) on an arity mismatch.
    bool resolve_trait_impl_type_args(MethodDecl& method_decl, const TraitTypeInfo& trait_info,
                                      Type* struct_type, SourceLocation loc, Span<Type*>& out);

    // validate_trait_implementations is split across the helpers below.
    struct TraitImplGroup {
        Type* struct_type;
        Type* trait_type;
        Span<Type*> trait_type_args;
        Vector<Decl*> impl_decls;
    };
    Vector<TraitImplGroup> group_pending_trait_impls();
    bool check_parent_trait_satisfied(const TraitImplGroup& group,
                                      const Vector<TraitImplGroup>& all_groups);
    void validate_and_register_impl_method(const TraitImplGroup& group, Decl* decl,
                                           Vector<bool>& implemented);
    void finalize_trait_impl(const TraitImplGroup& group, const Vector<bool>& implemented);

    void inject_default_method(Type* struct_type, Type* trait_type,
                               TraitMethodInfo& tmi, Span<Type*> trait_type_args);

    // Concretize an abstract trait-method type (`Self` -> the implementing
    // struct; a trait type-param -> the matching `for Trait<Args>` argument).
    // (Distinct from resolve_trait_method_type_expr, which works on TypeExprs.)
    Type* concretize_trait_type(Type* abstract_type, Type* struct_type, Span<Type*> trait_type_args);

    // Pending trait implementations (struct_name resolved to struct type + trait decl)
    struct PendingTraitImpl {
        Decl* decl;
        Type* struct_type;
        Type* trait_type;
        Span<Type*> trait_type_args;   // Resolved type args for generic traits
    };

    SemaContext& m_context;  // TypeExpr resolution service (resolve_type_expr)
    BumpAllocator& m_allocator;
    TypeEnv& m_type_env;
    TypeCache& m_types;
    SymbolTable& m_symbols;
    ErrorReporter& m_reporter;
    TypeChecker& m_checker;
    // The analyzer's synthetic-decl list (shared by reference): injected
    // default methods land here so the IR builder picks their bodies up.
    Vector<Decl*>& m_synthetic_decls;

    Vector<PendingTraitImpl> m_pending_trait_impls;
};

}

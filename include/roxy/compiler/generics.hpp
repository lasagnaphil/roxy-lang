#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Resolved bounds for all type params of one generic template
struct ResolvedTypeParams {
    Span<Span<TraitBound>> param_bounds;  // param_bounds[i] = bounds for i-th type param
};

// Maps type parameter names to concrete types for substitution
struct TypeSubstitution {
    Span<StringView> param_names;    // ["T", "U"]
    Span<Type*> concrete_types;      // [i32_type, string_type]

    Type* lookup(StringView name) const;
};

// A concrete instantiation of a generic function
struct GenericFunInstance {
    StringView mangled_name;         // "identity$i32"
    Decl* original_decl;             // Original generic FunDecl
    TypeSubstitution substitution;
    Decl* instantiated_decl;         // Cloned + substituted AST
    bool is_analyzed;
};

// A concrete instantiation of a generic struct
struct GenericStructInstance {
    StringView mangled_name;         // "Box$i32"
    Decl* original_decl;             // Original generic StructDecl
    TypeSubstitution substitution;
    Decl* instantiated_decl;         // Cloned + substituted StructDecl
    Type* concrete_type;             // The concrete struct Type*
    bool is_analyzed;
    Vector<Decl*> instantiated_methods;       // Cloned external method DeclMethod nodes
    Vector<Decl*> instantiated_constructors;  // Cloned external constructor DeclConstructor nodes
    Vector<Decl*> instantiated_destructors;   // Cloned external destructor DeclDestructor nodes
};

// Manages generic templates and their instantiations
class GenericInstantiator {
public:
    explicit GenericInstantiator(BumpAllocator& allocator, TypeCache& types);

    // Register generic templates
    void register_generic_fun(StringView name, Decl* decl);
    void register_generic_struct(StringView name, Decl* decl);

    // Register/query external method templates for generic structs
    void register_generic_struct_method(StringView struct_name, Decl* method_decl);
    const Vector<Decl*>* get_generic_struct_methods(StringView struct_name) const;

    // Register/query external constructor/destructor templates for generic structs
    void register_generic_struct_constructor(StringView struct_name, Decl* ctor_decl);
    void register_generic_struct_destructor(StringView struct_name, Decl* dtor_decl);
    const Vector<Decl*>* get_generic_struct_constructors(StringView struct_name) const;
    const Vector<Decl*>* get_generic_struct_destructors(StringView struct_name) const;

    // Query
    bool is_generic_fun(StringView name) const;
    bool is_generic_struct(StringView name) const;
    Decl* get_generic_fun_decl(StringView name) const;
    Decl* get_generic_struct_decl(StringView name) const;

    // Instantiate a generic function with concrete type arguments
    // Returns the mangled name for the instantiation
    StringView instantiate_fun(StringView name, Span<Type*> type_args);

    // Instantiate a generic struct with concrete type arguments
    // Returns the mangled name for the instantiation
    StringView instantiate_struct(StringView name, Span<Type*> type_args);

    // Pending instances that need semantic analysis
    bool has_pending_funs() const;
    Vector<GenericFunInstance*> take_pending_funs();

    bool has_pending_structs() const;
    Vector<GenericStructInstance*> take_pending_structs();

    // Access all instances (for IR builder)
    const Vector<GenericFunInstance*>& all_fun_instances() const { return m_all_fun_instances; }
    const Vector<GenericStructInstance*>& all_struct_instances() const { return m_all_struct_instances; }

    // Look up existing instances by mangled name
    GenericFunInstance* find_fun_instance(StringView mangled_name) const;
    GenericStructInstance* find_struct_instance(StringView mangled_name) const;

    // Look up a struct instance by its concrete Type* pointer
    GenericStructInstance* find_struct_instance_by_type(Type* concrete_type) const;

    // Name mangling
    StringView mangle_name(StringView base_name, Span<Type*> type_args);

    // Trait bounds storage for generic templates
    void set_fun_bounds(StringView name, ResolvedTypeParams bounds);
    void set_struct_bounds(StringView name, ResolvedTypeParams bounds);
    const ResolvedTypeParams* get_fun_bounds(StringView name) const;
    const ResolvedTypeParams* get_struct_bounds(StringView name) const;

    // Accessors for iterating all generic templates
    const tsl::robin_map<StringView, Decl*>& generic_funs_map() const { return m_generic_funs; }
    const tsl::robin_map<StringView, Decl*>& generic_structs_map() const { return m_generic_structs; }

    // Public AST cloning with type substitution (used by trait default method injection)
    Stmt* clone_stmt(Stmt* stmt, const TypeSubstitution& subst);
    TypeExpr* substitute_type_expr(TypeExpr* type_expr, const TypeSubstitution& subst);

private:
    // Clone AST with type substitution
    Decl* clone_fun_decl(Decl* original, const TypeSubstitution& subst, StringView new_name);
    Decl* clone_struct_decl(Decl* original, const TypeSubstitution& subst, StringView new_name);
    Decl* clone_method_decl(Decl* original, const TypeSubstitution& subst, StringView mangled_struct_name);
    Decl* clone_constructor_decl(Decl* original, const TypeSubstitution& subst, StringView mangled_struct_name);
    Decl* clone_destructor_decl(Decl* original, const TypeSubstitution& subst, StringView mangled_struct_name);
    Expr* clone_expr(Expr* expr, const TypeSubstitution& subst);
    Decl* clone_decl(Decl* decl, const TypeSubstitution& subst);
    Span<Decl*> clone_decl_list(Span<Decl*> decls, const TypeSubstitution& subst);
    Span<CallArg> clone_call_args(Span<CallArg> args, const TypeSubstitution& subst);
    Span<FieldInit> clone_field_inits(Span<FieldInit> fields, const TypeSubstitution& subst);

    // Get the type name string for mangling
    StringView type_name_for_mangling(Type* type);

    // Convert a resolved Type* back into a TypeExpr for AST substitution
    TypeExpr* type_to_type_expr(Type* type, SourceLocation loc);

    BumpAllocator& m_allocator;
    TypeCache& m_types;

    // Generic function templates
    tsl::robin_map<StringView, Decl*> m_generic_funs;

    // Generic struct templates
    tsl::robin_map<StringView, Decl*> m_generic_structs;

    // External method templates for generic structs (struct_name -> list of DeclMethod)
    tsl::robin_map<StringView, Vector<Decl*>> m_generic_struct_methods;

    // External constructor/destructor templates for generic structs
    tsl::robin_map<StringView, Vector<Decl*>> m_generic_struct_constructors;
    tsl::robin_map<StringView, Vector<Decl*>> m_generic_struct_destructors;

    // All function instances (including already analyzed ones)
    Vector<GenericFunInstance*> m_all_fun_instances;

    // All struct instances
    Vector<GenericStructInstance*> m_all_struct_instances;

    // Pending instances that need analysis
    Vector<GenericFunInstance*> m_pending_funs;
    Vector<GenericStructInstance*> m_pending_structs;

    // Cache: mangled_name -> instance (to avoid duplicate instantiation)
    tsl::robin_map<StringView, GenericFunInstance*> m_fun_instance_cache;
    tsl::robin_map<StringView, GenericStructInstance*> m_struct_instance_cache;

    // Resolved trait bounds for generic templates
    tsl::robin_map<StringView, ResolvedTypeParams> m_fun_bounds;
    tsl::robin_map<StringView, ResolvedTypeParams> m_struct_bounds;
};

}

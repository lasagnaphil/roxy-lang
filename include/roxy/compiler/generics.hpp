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
};

// Manages generic templates and their instantiations
class GenericInstantiator {
public:
    explicit GenericInstantiator(BumpAllocator& allocator, TypeCache& types);

    // Register generic templates
    void register_generic_fun(StringView name, Decl* decl);
    void register_generic_struct(StringView name, Decl* decl);

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

    // Public AST cloning with type substitution (used by trait default method injection)
    Stmt* clone_stmt(Stmt* stmt, const TypeSubstitution& subst);
    TypeExpr* substitute_type_expr(TypeExpr* type_expr, const TypeSubstitution& subst);

private:
    // Clone AST with type substitution
    Decl* clone_fun_decl(Decl* original, const TypeSubstitution& subst, StringView new_name);
    Decl* clone_struct_decl(Decl* original, const TypeSubstitution& subst, StringView new_name);
    Expr* clone_expr(Expr* expr, const TypeSubstitution& subst);
    Decl* clone_decl(Decl* decl, const TypeSubstitution& subst);
    Span<Decl*> clone_decl_list(Span<Decl*> decls, const TypeSubstitution& subst);
    Span<CallArg> clone_call_args(Span<CallArg> args, const TypeSubstitution& subst);
    Span<FieldInit> clone_field_inits(Span<FieldInit> fields, const TypeSubstitution& subst);

    // Get the type name string for mangling
    StringView type_name_for_mangling(Type* type);

    BumpAllocator& m_allocator;
    TypeCache& m_types;

    // Generic function templates
    tsl::robin_map<StringView, Decl*, StringViewHash, StringViewEqual> m_generic_funs;

    // Generic struct templates
    tsl::robin_map<StringView, Decl*, StringViewHash, StringViewEqual> m_generic_structs;

    // All function instances (including already analyzed ones)
    Vector<GenericFunInstance*> m_all_fun_instances;

    // All struct instances
    Vector<GenericStructInstance*> m_all_struct_instances;

    // Pending instances that need analysis
    Vector<GenericFunInstance*> m_pending_funs;
    Vector<GenericStructInstance*> m_pending_structs;

    // Cache: mangled_name -> instance (to avoid duplicate instantiation)
    tsl::robin_map<StringView, GenericFunInstance*, StringViewHash, StringViewEqual> m_fun_instance_cache;
    tsl::robin_map<StringView, GenericStructInstance*, StringViewHash, StringViewEqual> m_struct_instance_cache;
};

}

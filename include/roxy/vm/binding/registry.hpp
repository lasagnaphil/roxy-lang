#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/vm/bytecode.hpp"
#include "roxy/vm/binding/type_traits.hpp"
#include "roxy/vm/binding/function_traits.hpp"
#include "roxy/vm/binding/binder.hpp"

#include "roxy/core/tsl/robin_map.h"

#include <cstdio>
#include <cstring>
#include <utility>

namespace rx {

// Forward declarations
class TypeEnv;
struct TypeExpr;

// Type kind enum for deferred type creation (used by NativeFieldEntry for struct field registration)
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,    // string
};

// What kind of method entry this is
enum class GenericMethodKind : u8 {
    Method,
    Constructor,
    Destructor,
};

// Field descriptor for native struct registration
struct NativeFieldEntry {
    const char* name;
    NativeTypeKind type_kind;
};

// Native struct registration entry
struct NativeStructEntry {
    StringView name;
    Vector<NativeFieldEntry> fields;
};

// Function pointer that resolves a C++ type to a Roxy Type* given a TypeCache.
// Used by auto-bound functions to defer type resolution to use-time.
using TypeResolverFn = Type* (*)(TypeCache&);

// How a native function's parameter/return types are described
enum class NativeTypeInfoMode : u8 {
    Resolver,   // TypeResolverFn deferred resolution — bind<>, bind_method<>
    Parsed,     // TypeExpr AST from string signature — bind_native(func, sig), bind_method(func, sig)
};

// Entry for a registered native function (unified for both concrete and generic types)
struct NativeFunctionEntry {
    StringView name;                           // Mangled name for methods (e.g., "Point$$sum")
    NativeFunction func;
    NativeTypeInfoMode type_info_mode = NativeTypeInfoMode::Parsed;
    // Type info path: Resolver (deferred type resolution for cross-TypeCache compatibility)
    Vector<TypeResolverFn> param_resolvers;
    TypeResolverFn return_resolver = nullptr;
    // Type info path: Parsed (TypeExpr AST from string signature)
    Span<TypeExpr*> param_type_exprs;          // TypeExpr for each parameter
    TypeExpr* return_type_expr = nullptr;      // TypeExpr for return type (nullptr = void)
    u32 param_count;
    u32 min_args;                              // = param_count normally; < param_count for optional params
    bool is_method;                            // True if this is a struct method
    GenericMethodKind method_kind;             // Method/Constructor/Destructor
    StringView struct_name;                    // Non-empty for methods
    StringView method_name;                    // Original unmangled method name

    // Resolve parameter/return types using the given TypeCache (for Resolver mode).
    Type* resolve_return_type(TypeCache& types) const;
    void resolve_param_types(TypeCache& types, Type** out_params) const;
};

// Resolved constructor info (returned by instantiate_generic_constructor)
struct ResolvedConstructor {
    StringView native_name;        // "List$$new"
    Span<Type*> param_types;       // concrete types, excluding self
    u32 min_args;
};

// A registered generic native type (e.g., List<T>)
struct NativeGenericTypeEntry {
    StringView name;                               // "List"
    u32 type_param_count;                          // 1
    Vector<StringView> type_param_names;           // ["T"] for List, ["K", "V"] for Map
    StringView alloc_native_name;                  // "list_alloc" (non-method allocator)
    StringView copy_native_name;                   // "list_copy" (copy constructor for value params)
};

// NativeRegistry provides unified registration for native functions
// at both compile-time (semantic analysis) and runtime (VM execution)
class NativeRegistry {
public:
    explicit NativeRegistry(BumpAllocator& allocator, TypeCache& types)
        : m_allocator(allocator)
        , m_types(types)
    {}

    // Register a C++ function with automatic wrapper generation.
    // The C++ function must take RoxyVM* as its first parameter.
    // Usage: registry.bind<my_function>("my_function")
    template<auto FnPtr>
    void bind(const char* name) {
        using Traits = FunctionPointerTraits<FnPtr>;

        NativeFunctionEntry entry;
        entry.name = make_string_view(name);
        entry.func = FunctionBinder<FnPtr>::get();
        entry.param_count = Traits::arity - 1;  // exclude RoxyVM*
        entry.min_args = entry.param_count;
        entry.type_info_mode = NativeTypeInfoMode::Resolver;
        entry.is_method = false;
        entry.method_kind = GenericMethodKind::Method;

        // Store resolver functions for deferred type resolution
        entry.return_resolver = &RoxyType<typename Traits::return_type>::get;
        entry.param_resolvers = get_param_resolvers_skip_first<typename Traits::args_tuple>(
            std::make_index_sequence<Traits::arity - 1>{});

        m_function_entries.push_back(entry);

        // Update lookup map
        m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
    }

    // Register a native function from a Roxy signature string.
    // Usage: registry.bind_native(func, "fun print(s: string)")
    void bind_native(NativeFunction func, const char* signature);

    // Register a native function with a name override (for $$-mangled names).
    // Usage: registry.bind_native("bool$$to_string", func, "fun to_string(val: bool): string")
    void bind_native(const char* override_name, NativeFunction func, const char* signature);

    // Register a native struct type
    void register_struct(const char* name, std::initializer_list<NativeFieldEntry> fields);

    // Auto-bind a method on a native struct (C++ function with automatic wrapper).
    // The C++ function must take (RoxyVM*, SelfPtr*, ...) as its first two parameters.
    // Usage: registry.bind_method<point_sum>("Point", "sum")
    template<auto FnPtr>
    void bind_method(const char* struct_name, const char* method_name) {
        using Traits = FunctionPointerTraits<FnPtr>;
        static_assert(Traits::arity >= 2,
                      "Method must have RoxyVM* and self pointer as first two parameters");

        NativeFunctionEntry entry;
        entry.struct_name = make_string_view(struct_name);
        entry.method_name = make_string_view(method_name);
        entry.name = mangle_method_name(entry.struct_name, entry.method_name);
        entry.func = FunctionBinder<FnPtr>::get();
        entry.param_count = Traits::arity - 2;  // Exclude RoxyVM* and self
        entry.min_args = entry.param_count;
        entry.type_info_mode = NativeTypeInfoMode::Resolver;
        entry.is_method = true;
        entry.method_kind = GenericMethodKind::Method;

        // Store resolver functions, skipping RoxyVM* and self
        entry.return_resolver = &RoxyType<typename Traits::return_type>::get;
        entry.param_resolvers = get_param_resolvers_skip_two<typename Traits::args_tuple>(
            std::make_index_sequence<Traits::arity - 2>{});

        m_function_entries.push_back(entry);
        m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
    }

    // Bind a method from a Roxy signature string (concrete or generic types).
    // Usage: registry.bind_method(func, "fun Point.product(): i32")
    //        registry.bind_method(func, "fun List<T>.push(val: T)")
    void bind_method(NativeFunction func, const char* signature);

    // Bind a constructor from a Roxy signature string.
    // Usage: registry.bind_constructor(func, "fun List<T>.new(cap: i32)", 0)
    void bind_constructor(NativeFunction func, const char* signature, u32 min_args = 0);

    // Register a generic native type from a type declaration string.
    // Usage: registry.register_generic_type("List<T>", "list_alloc", func)
    void register_generic_type(const char* type_decl,
                               const char* alloc_name, NativeFunction alloc_func);

    // Bind a destructor on a generic native type (receives self as first arg, no other params)
    void bind_generic_destructor(const char* type_name, NativeFunction func);

    // Bind a copy constructor on a generic native type (for deep-copy on value parameter passing)
    void bind_generic_copy_constructor(const char* type_name,
                                       const char* copy_func_name,
                                       NativeFunction func);

    bool has_generic_type(StringView name) const;

    StringView get_generic_alloc_name(StringView name) const;
    StringView get_generic_copy_name(StringView name) const;

    // Returns only kind==Method entries as Span<MethodInfo>
    Span<MethodInfo> instantiate_generic_methods(StringView name, Span<Type*> type_args,
                                                  BumpAllocator& allocator, TypeCache& types) const;

    // Returns ResolvedConstructor (empty native_name if none registered)
    ResolvedConstructor instantiate_generic_constructor(StringView name, Span<Type*> type_args,
                                                         BumpAllocator& allocator,
                                                         TypeCache& types) const;

    // Create struct types from registered native structs and add to TypeEnv/SymbolTable
    void apply_structs_to_types(TypeEnv& type_env, BumpAllocator& allocator, SymbolTable& symbols);

    // Add method entries to their struct types (call after apply_structs_to_types)
    void apply_methods_to_types(TypeEnv& type_env, BumpAllocator& allocator);

    // Apply registered functions to symbol table for semantic analysis
    void apply_to_symbols(SymbolTable& symbols, TypeCache& types, BumpAllocator& allocator);

    // Overload for backwards compatibility (uses registry's own TypeCache)
    void apply_to_symbols(SymbolTable& symbols);

    // Apply registered functions to bytecode module for runtime
    void apply_to_module(BCModule* module);

    // Get native function index by name (-1 if not found)
    i32 get_index(StringView name) const;

    // Check if a name is a registered native function
    bool is_native(StringView name) const;

    // Get the number of registered functions
    u32 size() const { return static_cast<u32>(m_function_entries.size()); }

    // Get entry by index
    const NativeFunctionEntry& get_entry(u32 index) const { return m_function_entries[index]; }

    // Access type cache
    TypeCache& types() { return m_types; }

    // Access struct entries (for SemanticAnalyzer to register named types)
    const Vector<NativeStructEntry>& struct_entries() const { return m_struct_entries; }

    // Copy all entries to another registry (for combining multiple registries)
    void copy_entries_to(NativeRegistry& other) const;

private:
    StringView make_string_view(const char* str) {
        u32 len = static_cast<u32>(strlen(str));
        return StringView(str, len);
    }

    // Mangle method name: "StructName$$method_name"
    StringView mangle_method_name(StringView struct_name, StringView method_name) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%.*s$$%.*s",
                 static_cast<int>(struct_name.size()), struct_name.data(),
                 static_cast<int>(method_name.size()), method_name.data());
        u32 len = static_cast<u32>(strlen(buf));
        char* ptr = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
        memcpy(ptr, buf, len + 1);
        return StringView(ptr, len);
    }

    // Extract param resolvers, skipping the first element (RoxyVM*)
    template<typename Tuple, std::size_t... Is>
    static Vector<TypeResolverFn> get_param_resolvers_skip_first(std::index_sequence<Is...>) {
        Vector<TypeResolverFn> resolvers;
        (resolvers.push_back(&RoxyType<std::tuple_element_t<Is + 1, Tuple>>::get), ...);
        return resolvers;
    }

    // Extract param resolvers, skipping the first two elements (RoxyVM* and self)
    template<typename Tuple, std::size_t... Is>
    static Vector<TypeResolverFn> get_param_resolvers_skip_two(std::index_sequence<Is...>) {
        Vector<TypeResolverFn> resolvers;
        (resolvers.push_back(&RoxyType<std::tuple_element_t<Is + 2, Tuple>>::get), ...);
        return resolvers;
    }

    // Parse a Roxy function/method signature string into a Decl AST node.
    // Prepends "native " and appends ";" then runs Lexer+Parser.
    Decl* parse_signature(const char* signature);

    // Parse "List<T>" or "Map<K, V>" → extract type name and param names.
    void parse_type_decl(const char* type_decl, StringView& out_name,
                         Vector<StringView>& out_param_names);

    // Resolve a TypeExpr to a Type*, substituting type param names with concrete type_args.
    // Also called from NativeFunctionEntry methods, so declared as a public static.
public:
    static Type* resolve_type_expr(TypeExpr* expr,
                                   Span<StringView> type_param_names,
                                   Span<Type*> type_args,
                                   TypeCache& types);
private:

    BumpAllocator& m_allocator;
    TypeCache& m_types;
    Vector<NativeFunctionEntry> m_function_entries;
    Vector<NativeStructEntry> m_struct_entries;
    tsl::robin_map<StringView, i32> m_name_to_index;
    tsl::robin_map<StringView, NativeGenericTypeEntry> m_generic_types;
};

}

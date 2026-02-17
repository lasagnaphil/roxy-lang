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

// Forward declaration
class TypeEnv;

// Type kind enum for deferred type creation
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,    // string
};

// Parameter descriptor for native type methods/constructors/destructors.
// Also used as the unified parameter description in NativeFunctionEntry (concrete_param wraps NativeTypeKind).
struct NativeParamDesc {
    bool is_type_param;
    NativeTypeKind kind;          // when !is_type_param
    u32 type_param_index;         // when is_type_param (0=T, 1=U, ...)
};

inline NativeParamDesc concrete_param(NativeTypeKind k) { return {false, k, 0}; }
inline NativeParamDesc type_param(u32 index) { return {true, NativeTypeKind::Void, index}; }

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

// Entry for a registered native function (unified for both concrete and generic types)
struct NativeFunctionEntry {
    StringView name;                           // Mangled name for methods (e.g., "Point$$sum")
    NativeFunction func;
    // Type info path 1 (is_manual=false): param_descs + return_desc
    Vector<NativeParamDesc> param_descs;       // Replaces param_type_kinds; supports type params for generics
    NativeParamDesc return_desc;               // Replaces return_type_kind
    // Type info path 2 (is_manual=true): param_types + return_type
    Vector<Type*> param_types;
    Type* return_type;
    u32 param_count;
    u32 min_args;                              // = param_count normally; < param_count for optional params
    bool is_manual;                            // True if registered with bind_manual
    bool is_method;                            // True if this is a struct method
    GenericMethodKind method_kind;             // Method/Constructor/Destructor
    StringView struct_name;                    // Non-empty for methods
    StringView method_name;                    // Original unmangled method name
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

    // Register a C++ function with automatic wrapper generation
    // Usage: registry.bind<my_function>("my_function")
    template<auto FnPtr>
    void bind(const char* name) {
        using Traits = FunctionPointerTraits<FnPtr>;

        NativeFunctionEntry entry;
        entry.name = make_string_view(name);
        entry.func = FunctionBinder<FnPtr>::get();
        entry.param_count = Traits::arity;
        entry.min_args = Traits::arity;
        entry.is_manual = false;
        entry.is_method = false;
        entry.method_kind = GenericMethodKind::Method;

        // Extract return type desc
        entry.return_desc = concrete_param(get_type_kind<typename Traits::return_type>());
        entry.return_type = nullptr;

        // Extract parameter descs
        entry.param_descs = get_param_descs<typename Traits::args_tuple>(
            std::make_index_sequence<Traits::arity>{});

        m_function_entries.push_back(entry);

        // Update lookup map
        m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
    }

    // Register a native function with manual wrapper (for functions needing VM access)
    void bind_manual(const char* name, NativeFunction func,
                     std::initializer_list<Type*> param_types, Type* return_type);

    // Register a native function with type kinds (for functions needing VM access)
    void bind_native(const char* name, NativeFunction func,
                     std::initializer_list<NativeTypeKind> param_type_kinds,
                     NativeTypeKind return_type_kind);

    // Register a native struct type
    void register_struct(const char* name, std::initializer_list<NativeFieldEntry> fields);

    // Auto-bind a method on a native struct (first parameter is self pointer)
    // Usage: registry.bind_method<point_sum>("Point", "sum")
    template<auto FnPtr>
    void bind_method(const char* struct_name, const char* method_name) {
        using Traits = FunctionPointerTraits<FnPtr>;
        static_assert(Traits::arity >= 1, "Method must have at least one parameter (self)");

        NativeFunctionEntry entry;
        entry.struct_name = make_string_view(struct_name);
        entry.method_name = make_string_view(method_name);
        entry.name = mangle_method_name(entry.struct_name, entry.method_name);
        entry.func = FunctionBinder<FnPtr>::get();
        entry.param_count = Traits::arity - 1;  // Exclude self from Roxy-visible param count
        entry.min_args = entry.param_count;
        entry.is_manual = false;
        entry.is_method = true;
        entry.method_kind = GenericMethodKind::Method;

        // Extract return type desc
        entry.return_desc = concrete_param(get_type_kind<typename Traits::return_type>());
        entry.return_type = nullptr;

        // Extract parameter descs, skipping self (index 0)
        entry.param_descs = get_method_param_descs<typename Traits::args_tuple>(
            std::make_index_sequence<Traits::arity - 1>{});

        m_function_entries.push_back(entry);
        m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
    }

    // Manual-bind a method with type kinds (for methods needing VM access)
    void bind_method_native(const char* struct_name, const char* method_name,
                            NativeFunction func,
                            std::initializer_list<NativeTypeKind> param_type_kinds,
                            NativeTypeKind return_type_kind);

    // Manual-bind a method with Type* (for methods needing VM access)
    void bind_method_manual(const char* struct_name, const char* method_name,
                            NativeFunction func,
                            std::initializer_list<Type*> param_types, Type* return_type);

    // Register a generic native type with its allocation function.
    void register_generic_type(const char* name, u32 type_param_count,
                               const char* alloc_name, NativeFunction alloc_func);

    // Bind a method on a generic native type (receives self as first arg)
    void bind_generic_method(const char* type_name, const char* method_name,
                             NativeFunction func,
                             std::initializer_list<NativeParamDesc> params,
                             NativeParamDesc return_desc);

    // Bind a constructor on a generic native type (receives self as first arg)
    void bind_generic_constructor(const char* type_name, NativeFunction func,
                                  u32 min_args,
                                  std::initializer_list<NativeParamDesc> params);

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

    // Map C++ types to NativeTypeKind
    template<typename T> static constexpr NativeTypeKind get_type_kind();

    template<typename Tuple, std::size_t... Is>
    Vector<NativeParamDesc> get_param_descs(std::index_sequence<Is...>) {
        Vector<NativeParamDesc> descs;
        (descs.push_back(concrete_param(get_type_kind<std::tuple_element_t<Is, Tuple>>())), ...);
        return descs;
    }

    // Extract param descs for method parameters, skipping self (index 0)
    template<typename Tuple, std::size_t... Is>
    Vector<NativeParamDesc> get_method_param_descs(std::index_sequence<Is...>) {
        Vector<NativeParamDesc> descs;
        (descs.push_back(concrete_param(get_type_kind<std::tuple_element_t<Is + 1, Tuple>>())), ...);
        return descs;
    }

    BumpAllocator& m_allocator;
    TypeCache& m_types;
    Vector<NativeFunctionEntry> m_function_entries;
    Vector<NativeStructEntry> m_struct_entries;
    tsl::robin_map<StringView, i32> m_name_to_index;
    tsl::robin_map<StringView, NativeGenericTypeEntry> m_generic_types;
};

// Template specializations for get_type_kind
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<void>() { return NativeTypeKind::Void; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<bool>() { return NativeTypeKind::Bool; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<i8>() { return NativeTypeKind::I8; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<i16>() { return NativeTypeKind::I16; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<i32>() { return NativeTypeKind::I32; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<i64>() { return NativeTypeKind::I64; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<u8>() { return NativeTypeKind::U8; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<u16>() { return NativeTypeKind::U16; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<u32>() { return NativeTypeKind::U32; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<u64>() { return NativeTypeKind::U64; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<f32>() { return NativeTypeKind::F32; }
template<> constexpr NativeTypeKind NativeRegistry::get_type_kind<f64>() { return NativeTypeKind::F64; }

}

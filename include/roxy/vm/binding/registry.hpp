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

#include <cstring>
#include <utility>

namespace rx {

// Type kind enum for deferred type creation
enum class NativeTypeKind : u8 {
    Void, Bool,
    I8, I16, I32, I64,
    U8, U16, U32, U64,
    F32, F64,
    String,    // string
};

// Entry for a registered native function
struct NativeEntry {
    StringView name;
    NativeFunction func;
    Vector<NativeTypeKind> param_type_kinds;  // Used for automatic binding
    Vector<Type*> param_types;                 // Used for manual binding
    NativeTypeKind return_type_kind;
    Type* return_type;                         // Used for manual binding
    u32 param_count;
    bool is_manual;                            // True if registered with bind_manual
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

        NativeEntry entry;
        entry.name = make_string_view(name);
        entry.func = FunctionBinder<FnPtr>::get();
        entry.param_count = Traits::arity;
        entry.is_manual = false;

        // Extract return type kind
        entry.return_type_kind = get_type_kind<typename Traits::return_type>();
        entry.return_type = nullptr;

        // Extract parameter type kinds
        entry.param_type_kinds = get_param_type_kinds<typename Traits::args_tuple>(
            std::make_index_sequence<Traits::arity>{});

        m_entries.push_back(entry);

        // Update lookup map
        m_name_to_index[entry.name] = static_cast<i32>(m_entries.size() - 1);
    }

    // Register a native function with manual wrapper (for functions needing VM access)
    // Uses Type* directly - only works if types come from the same TypeCache used during compilation
    // Usage: registry.bind_manual("native_fn", native_fn_wrapper, {param_types}, return_type)
    void bind_manual(const char* name, NativeFunction func,
                     std::initializer_list<Type*> param_types, Type* return_type) {
        NativeEntry entry;
        entry.name = make_string_view(name);
        entry.func = func;
        entry.return_type = return_type;
        entry.return_type_kind = NativeTypeKind::Void;  // Not used for manual
        entry.param_count = static_cast<u32>(param_types.size());
        entry.is_manual = true;

        for (Type* t : param_types) {
            entry.param_types.push_back(t);
        }

        m_entries.push_back(entry);
        m_name_to_index[entry.name] = static_cast<i32>(m_entries.size() - 1);
    }

    // Register a native function with type kinds (for functions needing VM access)
    // This allows the types to be recreated in any TypeCache
    // Usage: registry.bind_native("native_fn", native_fn_wrapper, {NativeTypeKind::I32}, NativeTypeKind::I32)
    void bind_native(const char* name, NativeFunction func,
                     std::initializer_list<NativeTypeKind> param_type_kinds,
                     NativeTypeKind return_type_kind) {
        NativeEntry entry;
        entry.name = make_string_view(name);
        entry.func = func;
        entry.return_type = nullptr;
        entry.return_type_kind = return_type_kind;
        entry.param_count = static_cast<u32>(param_type_kinds.size());
        entry.is_manual = false;  // Use type kinds, not stored Type*

        for (NativeTypeKind k : param_type_kinds) {
            entry.param_type_kinds.push_back(k);
        }

        m_entries.push_back(entry);
        m_name_to_index[entry.name] = static_cast<i32>(m_entries.size() - 1);
    }

    // Apply registered functions to symbol table for semantic analysis
    // Uses the provided TypeCache and allocator to create types
    void apply_to_symbols(SymbolTable& symbols, TypeCache& types, BumpAllocator& allocator) {
        for (const auto& entry : m_entries) {
            // Get or create return type
            Type* ret_type = entry.is_manual ? entry.return_type
                                              : type_from_kind(entry.return_type_kind, types);

            // Allocate param types array
            Type** param_array = nullptr;
            if (entry.param_count > 0) {
                param_array = reinterpret_cast<Type**>(
                    allocator.alloc_bytes(sizeof(Type*) * entry.param_count, alignof(Type*)));
                for (u32 i = 0; i < entry.param_count; i++) {
                    if (entry.is_manual) {
                        param_array[i] = entry.param_types[i];
                    } else {
                        param_array[i] = type_from_kind(entry.param_type_kinds[i], types);
                    }
                }
            }

            // Create function type using provided TypeCache
            Type* func_type = types.function_type(
                Span<Type*>(param_array, entry.param_count), ret_type);

            // Define in symbol table
            symbols.define(SymbolKind::Function, entry.name, func_type,
                          SourceLocation{0, 0}, nullptr);
        }
    }

    // Overload for backwards compatibility (uses registry's own TypeCache)
    void apply_to_symbols(SymbolTable& symbols) {
        apply_to_symbols(symbols, m_types, m_allocator);
    }

    // Apply registered functions to bytecode module for runtime
    void apply_to_module(BCModule* module) {
        for (const auto& entry : m_entries) {
            BCNativeFunction bc_native;
            bc_native.name = entry.name;
            bc_native.func = entry.func;
            bc_native.param_count = entry.param_count;
            module->native_functions.push_back(bc_native);
        }
    }

    // Get native function index by name (-1 if not found)
    i32 get_index(StringView name) const {
        auto it = m_name_to_index.find(name);
        if (it != m_name_to_index.end()) {
            return it->second;
        }
        return -1;
    }

    // Check if a name is a registered native function
    bool is_native(StringView name) const {
        return m_name_to_index.find(name) != m_name_to_index.end();
    }

    // Get the number of registered functions
    u32 size() const { return static_cast<u32>(m_entries.size()); }

    // Get entry by index
    const NativeEntry& get_entry(u32 index) const { return m_entries[index]; }

    // Access type cache
    TypeCache& types() { return m_types; }

    // Copy all entries to another registry (for combining multiple registries)
    void copy_entries_to(NativeRegistry& other) const {
        for (const auto& entry : m_entries) {
            // Copy the entry - use bind_native for type-kind based entries
            if (!entry.is_manual && entry.param_type_kinds.size() > 0) {
                // Use bind_native to register with type kinds
                NativeEntry new_entry;
                new_entry.name = entry.name;
                new_entry.func = entry.func;
                new_entry.return_type = nullptr;
                new_entry.return_type_kind = entry.return_type_kind;
                new_entry.param_count = entry.param_count;
                new_entry.is_manual = false;
                new_entry.param_type_kinds = entry.param_type_kinds;  // Copy vector
                other.m_entries.push_back(new_entry);
                other.m_name_to_index[new_entry.name] = static_cast<i32>(other.m_entries.size() - 1);
            } else {
                // Direct copy for manual entries or simple auto-bound entries
                other.m_entries.push_back(entry);
                other.m_name_to_index[entry.name] = static_cast<i32>(other.m_entries.size() - 1);
            }
        }
    }

private:
    StringView make_string_view(const char* str) {
        u32 len = static_cast<u32>(strlen(str));
        return StringView(str, len);
    }

    // Map C++ types to NativeTypeKind
    template<typename T> static constexpr NativeTypeKind get_type_kind();

    template<typename Tuple, std::size_t... Is>
    Vector<NativeTypeKind> get_param_type_kinds(std::index_sequence<Is...>) {
        Vector<NativeTypeKind> kinds;
        (kinds.push_back(get_type_kind<std::tuple_element_t<Is, Tuple>>()), ...);
        return kinds;
    }

    // Convert NativeTypeKind to actual Type*
    static Type* type_from_kind(NativeTypeKind kind, TypeCache& types) {
        switch (kind) {
            case NativeTypeKind::Void: return types.void_type();
            case NativeTypeKind::Bool: return types.bool_type();
            case NativeTypeKind::I8: return types.i8_type();
            case NativeTypeKind::I16: return types.i16_type();
            case NativeTypeKind::I32: return types.i32_type();
            case NativeTypeKind::I64: return types.i64_type();
            case NativeTypeKind::U8: return types.u8_type();
            case NativeTypeKind::U16: return types.u16_type();
            case NativeTypeKind::U32: return types.u32_type();
            case NativeTypeKind::U64: return types.u64_type();
            case NativeTypeKind::F32: return types.f32_type();
            case NativeTypeKind::F64: return types.f64_type();
            case NativeTypeKind::String: return types.string_type();
            default: return types.error_type();
        }
    }

    BumpAllocator& m_allocator;
    TypeCache& m_types;
    Vector<NativeEntry> m_entries;
    tsl::robin_map<StringView, i32, StringViewHash, StringViewEqual> m_name_to_index;
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

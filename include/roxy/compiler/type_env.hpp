#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/generics.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// TypeEnv consolidates persistent type-system state that was previously
// scattered across TypeCache, SemanticAnalyzer, and Compiler.
// It owns the TypeCache, named/trait type registries, generics, and
// the builtin Printable trait pointer.
class TypeEnv {
public:
    explicit TypeEnv(BumpAllocator& allocator);

    TypeCache& types() { return m_types; }
    const TypeCache& types() const { return m_types; }

    void register_named_type(StringView name, Type* type);
    Type* named_type_by_name(StringView name);

    void register_trait_type(StringView name, Type* type);
    Type* trait_type_by_name(StringView name);

    // Searches primitives first, then named types
    Type* type_by_name(StringView name);

    Type* printable_type() const { return m_printable_type; }
    void set_printable_type(Type* type) { m_printable_type = type; }

    Type* hash_type() const { return m_hash_type; }
    void set_hash_type(Type* type) { m_hash_type = type; }

    GenericInstantiator& generics() { return m_generics; }
    const GenericInstantiator& generics() const { return m_generics; }

private:
    TypeCache m_types;
    GenericInstantiator m_generics;
    tsl::robin_map<StringView, Type*> m_named_types;
    tsl::robin_map<StringView, Type*> m_trait_types;
    Type* m_printable_type = nullptr;
    Type* m_hash_type = nullptr;
};

}

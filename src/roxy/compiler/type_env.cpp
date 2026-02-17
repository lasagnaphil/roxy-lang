#include "roxy/compiler/type_env.hpp"

namespace rx {

TypeEnv::TypeEnv(BumpAllocator& allocator)
    : m_types(allocator)
    , m_generics(allocator, m_types)
{
}

void TypeEnv::register_named_type(StringView name, Type* type) {
    m_named_types[name] = type;
}

Type* TypeEnv::named_type_by_name(StringView name) {
    auto it = m_named_types.find(name);
    if (it != m_named_types.end()) {
        return it->second;
    }
    return nullptr;
}

void TypeEnv::register_trait_type(StringView name, Type* type) {
    m_trait_types[name] = type;
}

Type* TypeEnv::trait_type_by_name(StringView name) {
    auto it = m_trait_types.find(name);
    if (it != m_trait_types.end()) {
        return it->second;
    }
    return nullptr;
}

Type* TypeEnv::type_by_name(StringView name) {
    // First try primitives
    Type* prim = m_types.primitive_by_name(name);
    if (prim) return prim;

    // Then try named types (structs, enums)
    return named_type_by_name(name);
}

}

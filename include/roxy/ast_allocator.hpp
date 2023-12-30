#pragma once

#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/rel_ptr.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/type.hpp"

namespace rx {

class AstAllocator {
private:
    static constexpr u64 s_initial_ast_allocator_capacity = 65536;

    BumpAllocator m_allocator;

    PrimitiveType* m_prim_types[(u32)PrimTypeKind::_size];

public:
    AstAllocator(u64 initial_capacity = s_initial_ast_allocator_capacity)
        : m_allocator(initial_capacity) {

        // Allocate every primitive type beforehand, these are going to get interned in the pool.
        for (u32 i = 0; i < (u32)PrimTypeKind::_size; i++) {
            m_prim_types[i] = m_allocator.emplace<PrimitiveType>((PrimTypeKind) i);
        }
    }

    template <typename T, typename ... Args, typename = std::enable_if_t<
            std::is_base_of_v<Expr, T> || std::is_base_of_v<Stmt, T> || std::is_base_of_v<Type, T> ||
            std::is_same_v<AstVarDecl, T>>>
    T* alloc(Args&&... args) {
        return m_allocator.emplace<T, Args...>(std::forward<Args>(args)...);
    }

    template <>
    PrimitiveType* alloc<PrimitiveType, PrimTypeKind>(PrimTypeKind&& prim_kind) {
        assert((u32)prim_kind < (u32)PrimTypeKind::_size);
        return m_prim_types[(u32)prim_kind];
    }

    PrimitiveType* get_void_type() { return m_prim_types[(u32)PrimTypeKind::Void]; }
    PrimitiveType* get_bool_type() { return m_prim_types[(u32)PrimTypeKind::Bool]; }
    PrimitiveType* get_string_type() { return m_prim_types[(u32)PrimTypeKind::String]; }

    template <typename T, typename U, typename = std::enable_if_t<std::is_convertible_v<U, T>>>
    Span<T> alloc_vector(Vector<U>&& vec) {
        void* raw_data = m_allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T));
        T* ptr = reinterpret_cast<T*>(raw_data);
        for (u32 i = 0; i < vec.size(); i++) {
            new (ptr + i) T(std::move(vec[i]));
        }
        return Span<T> {ptr, vec.size()};
    }
};

}
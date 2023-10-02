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
    BumpAllocator m_allocator;

    PrimitiveType* m_prim_type_bool;
    PrimitiveType* m_prim_type_number;
    PrimitiveType* m_prim_type_string;

public:
    AstAllocator(u64 initial_capacity) : m_allocator(initial_capacity) {
        m_prim_type_bool = m_allocator.emplace<PrimitiveType>(PrimTypeKind::Bool);
        m_prim_type_number = m_allocator.emplace<PrimitiveType>(PrimTypeKind::Number);
        m_prim_type_string = m_allocator.emplace<PrimitiveType>(PrimTypeKind::String);
    }

    template <typename T, typename ... Args, typename = std::enable_if_t<
            std::is_base_of_v<Expr, T> || std::is_base_of_v<Stmt, T> || std::is_base_of_v<Type, T>>>
    T* alloc(Args&&... args) {
        return m_allocator.emplace<T, Args...>(std::forward<Args>(args)...);
    }

    template <>
    PrimitiveType* alloc<PrimitiveType, PrimTypeKind>(PrimTypeKind&& prim_kind) {
        switch (prim_kind) {
            case PrimTypeKind::Bool: return m_prim_type_bool;
            case PrimTypeKind::Number: return m_prim_type_number;
            case PrimTypeKind::String: return m_prim_type_string;
        }
    }

    PrimitiveType* get_bool_type() { return m_prim_type_bool; }
    PrimitiveType* get_number_type() { return m_prim_type_number; }
    PrimitiveType* get_string_type() { return m_prim_type_string; }

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
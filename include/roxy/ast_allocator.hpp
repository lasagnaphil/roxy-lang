#pragma once

#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/expr.hpp"
#include "roxy/stmt.hpp"
#include "roxy/type.hpp"

namespace rx {

class AstAllocator {
private:
    BumpAllocator m_allocator;

    PrimitiveType s_prim_type_bool = {PrimTypeKind::Bool};
    PrimitiveType s_prim_type_number = {PrimTypeKind::Number};
    PrimitiveType s_prim_type_string = {PrimTypeKind::String};

public:
    AstAllocator(u64 initial_capacity) : m_allocator(initial_capacity) {}

    template <typename T, typename ... Args, typename = std::enable_if_t<
            std::is_base_of_v<Expr, T> || std::is_base_of_v<Stmt, T> || std::is_base_of_v<Type, T>>>
    T* alloc(Args&&... args) {
        return m_allocator.emplace<T, Args...>(std::forward<Args>(args)...);
    }

    template <>
    PrimitiveType* alloc<PrimitiveType, PrimTypeKind>(PrimTypeKind&& prim_kind) {
        switch (prim_kind) {
            case PrimTypeKind::Bool: return &s_prim_type_bool;
            case PrimTypeKind::Number: return &s_prim_type_number;
            case PrimTypeKind::String: return &s_prim_type_string;
        }
    }

    template <typename T>
    Span<T> copy(Vector<T>&& vec) {
        void* data = m_allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T));
        Span<T> span = {reinterpret_cast<T*>(data), vec.size()};
        for (u32 i = 0; i < vec.size(); i++) {
            new (span.data() + i) T(std::move(vec[i]));
        }
        return span;
    }
};

}
#pragma once

#include "roxy/core/types.hpp"
#include "roxy/token.hpp"

struct ObjString;

namespace rx {

enum class TypeKind : u8 {
    Primitive,
    Struct,
    Function,
};

// Primitive types
enum class PrimTypeKind : u8 {
    Bool, Number, String,
};

struct VarDecl;
struct PrimitiveType;

struct Type {
    TypeKind kind;

    Type(TypeKind kind) : kind(kind) {}

    bool is_bool() const;
    bool is_number() const;
    bool is_string() const;

    template <typename TypeT, typename = std::enable_if_t<std::is_base_of_v<Type, TypeT>>>
    const TypeT& cast() const {
        assert(kind == TypeT::s_kind);
        return static_cast<const TypeT&>(*this);
    }

    template <typename TypeT, typename = std::enable_if_t<std::is_base_of_v<Type, TypeT>>>
    TypeT& cast() {
        assert(kind == TypeT::s_kind);
        return static_cast<TypeT&>(*this);
    }

    template <typename TypeT, typename = std::enable_if_t<std::is_base_of_v<Type, TypeT>>>
    const TypeT* try_cast() const {
        if (kind == TypeT::s_kind) return static_cast<const TypeT*>(this);
        else return nullptr;
    }

    template <typename TypeT, typename = std::enable_if_t<std::is_base_of_v<Type, TypeT>>>
    TypeT* try_cast() {
        if (kind == TypeT::s_kind) return static_cast<TypeT*>(this);
        else return nullptr;
    }
};

struct VarDecl {
    Token name;
    Type* type; // can be null (inferred later)

    VarDecl() : name(), type(nullptr) {}
    VarDecl(Token name, Type* type) : name(name), type(type) {}
};

struct PrimitiveType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Primitive;

    PrimTypeKind prim_kind;

    PrimitiveType(PrimTypeKind prim_kind) : Type(s_kind), prim_kind(prim_kind) {}
};

struct StructType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Struct;

    Token name;
    Span<VarDecl> decl;

    StructType(Token name, Span<VarDecl> decl) : Type(s_kind), name(name), decl(decl) {}
};

struct FunctionType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Function;

    Type* ret;
    Span<Type*> params;

    FunctionType(Type* ret, Span<Type*> params) : Type(s_kind), ret(ret), params(params) {}
};

inline bool Type::is_bool() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::Bool;
}

inline bool Type::is_number() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::Number;
}

inline bool Type::is_string() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::String;
}

}
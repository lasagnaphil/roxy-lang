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

struct Type {
    TypeKind kind;

    Type(TypeKind kind) : kind(kind) {}

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
    Vector<VarDecl> decl;

    StructType(Token name, Vector<VarDecl>&& decl) : Type(s_kind), name(name), decl(decl) {}
};

struct FunctionType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Function;

    Type* ret;
    Vector<Type*> params;

    FunctionType(Type* ret, Vector<Type*>&& params) : Type(s_kind), ret(ret), params(params) {}
};

}
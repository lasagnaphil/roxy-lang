#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/rel_ptr.hpp"
#include "roxy/token.hpp"

struct ObjString;

namespace rx {

enum class TypeKind : u8 {
    Primitive,
    Struct,
    Function,
    Unassigned,
    Inferred,
};

// Primitive types
enum class PrimTypeKind : u8 {
    Void,
    Bool,
    // Signed integers
    U8, U16, U32, U64,
    // Unsigned integers
    I8, I16, I32, I64,
    // Floating point values
    F32, F64,
    // String (contents on the heap, but it's primitive since it's interned)
    String,
    _size
};

struct VarDecl;
struct PrimitiveType;

struct Type {
#ifndef NDEBUG
    virtual ~Type() = default;
#endif

    TypeKind kind;
    u16 size;
    u16 alignment;

    Type(TypeKind kind) : kind(kind) {}

    bool is_void() const;
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

struct AstVarDecl {
    Token name;
    RelPtr<Type> type;
    u32 local_index = 0;

    AstVarDecl(VarDecl var_decl) : name(var_decl.name), type(var_decl.type) {}
    AstVarDecl(Token name, Type* type) : name(name), type(type) {}
};

struct PrimitiveType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Primitive;

    PrimTypeKind prim_kind;

    static u16 s_prim_type_sizes[(u32)PrimTypeKind::_size];

    PrimitiveType(PrimTypeKind prim_kind) : Type(s_kind), prim_kind(prim_kind) {
        size = alignment = s_prim_type_sizes[(u32)prim_kind];
    }

    bool is_signed_integer() const {
        return prim_kind >= PrimTypeKind::I8 && prim_kind <= PrimTypeKind::I64;
    }

    bool is_unsigned_integer() const {
        return prim_kind >= PrimTypeKind::U8 && prim_kind <= PrimTypeKind::U64;
    }

    bool is_floating_point_num() const {
        return prim_kind == PrimTypeKind::F32 || prim_kind == PrimTypeKind::F64;
    }
};

struct StructType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Struct;

    Token name;
    RelSpan<AstVarDecl> declarations;

    StructType(Token name, Span<AstVarDecl> declarations) : Type(s_kind), name(name), declarations(declarations) {
        size = alignment = 0;
    }
};

struct FunctionType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Function;

    RelSpan<RelPtr<Type>> params;
    RelPtr<Type> ret;

    FunctionType(Span<RelPtr<Type>> params, Type* ret) : Type(s_kind), params(params), ret(ret) {
        size = alignment = 8;
    }
};

struct UnassignedType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Unassigned;

    Token name;

    UnassignedType(Token name) : Type(s_kind), name(name) {
        size = alignment = 0;
    }
};

struct InferredType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Inferred;

    InferredType() : Type(s_kind) {}
};

inline bool Type::is_void() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::Void;
}

inline bool Type::is_bool() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::Bool;
}

inline bool Type::is_number() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type &&
        prim_type->prim_kind >= PrimTypeKind::U8 &&
        prim_type->prim_kind <= PrimTypeKind::F64;
}

inline bool Type::is_string() const {
    auto prim_type = try_cast<PrimitiveType>();
    return prim_type && prim_type->prim_kind == PrimTypeKind::String;
}

}
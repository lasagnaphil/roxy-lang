#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/rel_ptr.hpp"
#include "roxy/token.hpp"
#include "roxy/opcode.hpp"
#include "roxy/il.hpp"

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
    U8,
    U16,
    U32,
    U64,
    // Unsigned integers
    I8,
    I16,
    I32,
    I64,
    // Floating point values
    F32,
    F64,
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

    // Added in sema analyzer

    // Local offset inside chunk (when this is a variable).
    u16 local_index = 0;

    // Offset inside struct (when this is a struct field declaration)
    // or offset inside param list (when this is a param declaration)
    u16 offset_bytes_from_parent = 0;

    ILAddress il_address;

    AstVarDecl(VarDecl var_decl) : name(var_decl.name), type(var_decl.type), il_address(ILAddress::make_invalid()) {}
    AstVarDecl(Token name, Type* type) : name(name), type(type), il_address(ILAddress::make_invalid()) {}
};

struct FunctionType;
struct FunctionStmt;

// TODO: Currently just assume that these function declarations are all inside the current module.
//  Later when we add a module system we need to change this...

struct FunDecl {
    Token name;
    Span<AstVarDecl> params;
    Type* ret_type;
    bool is_native;

    FunDecl() = default;
    FunDecl(Token name, Span<AstVarDecl> params, Type* ret_type, bool is_native) :
        name(name), params(params), ret_type(ret_type), is_native(is_native) {}
};

class Module;

struct AstFunDecl {
    Token name;
    RelSpan<AstVarDecl> params;
    RelPtr<Type> ret_type;
    bool is_native;

    // Added in sema analyzer
    RelPtr<FunctionType> type;
    u16 local_index = 0;
    std::string_view module;

    AstFunDecl(FunDecl fun_decl) :
        name(fun_decl.name), params(fun_decl.params), ret_type(fun_decl.ret_type),
        is_native(fun_decl.is_native), type(nullptr) {}
    AstFunDecl(Token name, Span<AstVarDecl> params, Type* ret_type, FunctionType* type, bool is_native) :
        name(name), params(params), ret_type(ret_type), type(type), is_native(is_native) {}
};

/*
struct NativeFunDecl {
    std::string name;
    Span<RelPtr<Type>> param_types;
    Type* ret_type;

    NativeFunDecl() = default;
    NativeFunDecl(std::string name, Span<RelPtr<Type>> param_types, Type* ret_type) :
        name(std::move(name)), param_types(param_types), ret_type(ret_type) {}
};

struct AstNativeFunDecl {
    std::string name;
    RelSpan<RelPtr<Type>> param_types;
    RelPtr<Type> ret_type;

    AstNativeFunDecl() = default;
    AstNativeFunDecl(NativeFunDecl fun_decl) :
        name(std::move(fun_decl.name)), param_types(fun_decl.param_types),  ret_type(fun_decl.ret_type) {}
};
 */

struct PrimitiveType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Primitive;

    PrimTypeKind prim_kind;

    static u16 s_prim_type_sizes[(u32)PrimTypeKind::_size];

    static u16 get_kind_size(PrimTypeKind prim_kind) {
        return s_prim_type_sizes[(u32)prim_kind];
    }
    static u16 get_kind_alignment(PrimTypeKind prim_kind) {
        return s_prim_type_sizes[(u32)prim_kind];
    }

    PrimitiveType(PrimTypeKind prim_kind) : Type(s_kind), prim_kind(prim_kind) {
        size = alignment = s_prim_type_sizes[(u32)prim_kind];
    }

    bool is_signed_integer() const {
        return prim_kind >= PrimTypeKind::I8 && prim_kind <= PrimTypeKind::I64;
    }

    bool is_unsigned_integer() const {
        return prim_kind >= PrimTypeKind::U8 && prim_kind <= PrimTypeKind::U64;
    }

    bool is_integer() const {
        return prim_kind == PrimTypeKind::I64 || prim_kind == PrimTypeKind::U64 ||
               prim_kind == PrimTypeKind::I32 || prim_kind == PrimTypeKind::U32 ||
               prim_kind == PrimTypeKind::I16 || prim_kind == PrimTypeKind::U16 ||
               prim_kind == PrimTypeKind::I8 || prim_kind == PrimTypeKind::U8;
    }

    bool is_floating_point_num() const {
        return prim_kind == PrimTypeKind::F32 || prim_kind == PrimTypeKind::F64;
    }

    bool is_4_bytes() const {
        return prim_kind == PrimTypeKind::I32 || prim_kind == PrimTypeKind::U32 || prim_kind == PrimTypeKind::F32;
    }

    bool is_8_bytes() const {
        return prim_kind == PrimTypeKind::I64 || prim_kind == PrimTypeKind::U64 ||
            prim_kind == PrimTypeKind::F64 || prim_kind == PrimTypeKind::String;
    }

    bool is_within_4_bytes() const {
        return prim_kind == PrimTypeKind::Bool ||
            prim_kind == PrimTypeKind::I32 || prim_kind == PrimTypeKind::U32 || prim_kind == PrimTypeKind::F32 ||
            prim_kind == PrimTypeKind::I16 || prim_kind == PrimTypeKind::U16 ||
            prim_kind == PrimTypeKind::I8 || prim_kind == PrimTypeKind::U8;
    }

    bool is_within_4_bytes_integer() const {
        return prim_kind == PrimTypeKind::Bool ||
            prim_kind == PrimTypeKind::I32 || prim_kind == PrimTypeKind::U32 ||
            prim_kind == PrimTypeKind::I16 || prim_kind == PrimTypeKind::U16 ||
            prim_kind == PrimTypeKind::I8 || prim_kind == PrimTypeKind::U8;
    }
};

struct StructType : public Type {
    static constexpr TypeKind s_kind = TypeKind::Struct;

    Token name;
    RelSpan<AstVarDecl> declarations;

    StructType(Token name, Span<AstVarDecl> declarations) : Type(s_kind), name(name), declarations(declarations) {
        size = 0;
        alignment = 0;
    }

    void calc_size_and_alignment() {
        for (auto& decl : declarations) {
            auto field_type = decl.type.get();
            u32 aligned_size = (size + field_type->alignment - 1) & ~(field_type->alignment - 1);
            size = aligned_size + field_type->size;
            alignment = field_type->alignment > alignment ? field_type->alignment : alignment;
            decl.offset_bytes_from_parent = aligned_size;
        }
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

constexpr ILOperator il_op_add(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::AddI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::AddL;
    case PrimTypeKind::F32:
        return ILOperator::AddF;
    case PrimTypeKind::F64:
        return ILOperator::AddD;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_sub(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::SubI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::SubL;
    case PrimTypeKind::F32:
        return ILOperator::SubF;
    case PrimTypeKind::F64:
        return ILOperator::SubD;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_mul(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
        return ILOperator::MulUI;
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::MulI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::MulL;
    case PrimTypeKind::F32:
        return ILOperator::MulF;
    case PrimTypeKind::F64:
        return ILOperator::MulD;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_div(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
        return ILOperator::DivUI;
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::DivI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::DivL;
    case PrimTypeKind::F32:
        return ILOperator::DivF;
    case PrimTypeKind::F64:
        return ILOperator::DivD;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_mod(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::ModI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::ModL;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_assign(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILOperator::AssignI;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILOperator::AssignL;
    case PrimTypeKind::F32:
        return ILOperator::AssignF;
    case PrimTypeKind::F64:
        return ILOperator::AssignD;
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILOperator il_op_binary(PrimTypeKind kind, TokenType type) {
    switch (type) {
    case TokenType::Plus:
        return il_op_add(kind);
    case TokenType::Minus:
        return il_op_sub(kind);
    case TokenType::Star:
        return il_op_mul(kind);
    case TokenType::Slash:
        return il_op_div(kind);
    case TokenType::Percent:
        return il_op_mod(kind);
    default:
        return ILOperator::Invalid;
    }
}

constexpr ILAddress make_const_value(PrimTypeKind kind) {
switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return ILAddress::make_const_int(0);
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return ILAddress::make_const_long(0);
    case PrimTypeKind::F32:
        return ILAddress::make_const_float(0);
    case PrimTypeKind::F64:
        return ILAddress::make_const_double(0);
    default:
        return ILAddress::make_invalid();
    }
}

}

#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/function_ref.hpp"
#include "roxy/opcode.hpp"
#include "roxy/type.hpp"

#include <string_view>

namespace rx {
class Obj;

struct TypeData {
    TypeKind kind;
    u16 size;
    u16 alignment;

    TypeData(const Type& type) : kind(type.kind), size(type.size), alignment(type.alignment) {}

    static UniquePtr<TypeData> from_type(const Type* type, const u8* source);
};

struct VarData {
    std::string name;
    UniquePtr<TypeData> type;

    VarData() = default;
    VarData(const AstVarDecl& decl, const u8* source) {
        name = decl.name.str(source);
        type = TypeData::from_type(decl.type.get(), source);
    }
};

struct PrimitiveTypeData : TypeData {
    static constexpr TypeKind s_kind = TypeKind::Primitive;

    PrimitiveTypeData() : TypeData(s_kind) {}
    PrimitiveTypeData(PrimTypeKind prim_kind) : TypeData(s_kind), prim_kind(prim_kind) {
        size = alignment = PrimitiveType::s_prim_type_sizes[(u32)prim_kind];
    }
    PrimitiveTypeData(const PrimitiveType& type) : TypeData(type), prim_kind(type.prim_kind) {}

    PrimTypeKind prim_kind;
};

struct StructTypeData : TypeData {
    static constexpr TypeKind s_kind = TypeKind::Struct;

    StructTypeData() : TypeData(s_kind) {}

    StructTypeData(const StructType& type, const u8* source) : TypeData(type), name(type.name.str(source)) {
        fields.resize(type.declarations.size());
        for (u32 i = 0; i < type.declarations.size(); i++) {
            fields[i] = VarData(type.declarations[i], source);
        }
    }

    std::string name;
    Vector<VarData> fields;
};

struct FunctionTypeData : TypeData {
    static constexpr TypeKind s_kind = TypeKind::Function;

    FunctionTypeData() : TypeData(s_kind) {}

    FunctionTypeData(const FunctionType& type, const u8* source) : TypeData(type) {
        params.resize(type.params.size());
        for (u32 i = 0; i < type.params.size(); i++) {
            params[i] = TypeData::from_type(type.params[i].get(), source);
        }
        ret = TypeData::from_type(type.ret.get(), source);
    }

    Vector<UniquePtr<TypeData>> params;
    UniquePtr<TypeData> ret;
};

inline UniquePtr<TypeData> TypeData::from_type(const Type* type, const u8* source) {
    if (type->kind == TypeKind::Primitive) {
        return UniquePtr<TypeData>(new PrimitiveTypeData(type->cast<PrimitiveType>()));
    }
    else if (type->kind == TypeKind::Struct) {
        return UniquePtr<TypeData>(new StructTypeData(type->cast<StructType>(), source));
    }
    else if (type->kind == TypeKind::Function) {
        return UniquePtr<TypeData>(new FunctionTypeData(type->cast<FunctionType>(), source));
    }
    else {
        return nullptr;
    }
}

struct LocalTableEntry {
    u16 start;
    u16 size;
    UniquePtr<TypeData> type;
    std::string name;
};

class Module;
class ArgStack;

using NativeFunctionRef = void(*)(ArgStack*);

struct Chunk {
    std::string m_name;
    Vector<u8> m_bytecode;
    Vector<LocalTableEntry> m_local_table;
    Chunk** m_function_table;
    NativeFunctionRef* m_native_function_table;
    Module* m_outer_module;

    // Line debug information
    Vector<u32> m_lines;

    Chunk(std::string name, Module* outer_module) : m_name(std::move(name)), m_outer_module(outer_module) {}

    void write(u8 byte, u32 line);

    u32 get_locals_slot_size() {
        if (m_local_table.empty()) return 0;
        auto& last_local = m_local_table[m_local_table.size() - 1];
        return last_local.start + last_local.size;
    }

    u32 get_line(u32 bytecode_offset);

    void print_disassembly();
    u32 disassemble_instruction(u32 offset);

private:
    u32 print_simple_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u8_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u16_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u32_instruction(OpCode opcode, u32 offset);
    u32 print_arg_u64_instruction(OpCode opcode, u32 offset);
    u32 print_arg_f32_instruction(OpCode opcode, u32 offset);
    u32 print_arg_f64_instruction(OpCode opcode, u32 offset);
    u32 print_branch_instruction(OpCode opcode, i32 sign, u32 offset);
    u32 print_branch_shortened_instruction(OpCode opcode, i32 sign, u32 offset);
    u32 print_string_instruction(OpCode opcode, u32 offset);

    u16 get_u16_from_bytecode_offset(u32 offset) {
        u16 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u16));
        return value;
    }

    u32 get_u32_from_bytecode_offset(u32 offset) {
        u32 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u32));
        return value;
    }

    u64 get_u64_from_bytecode_offset(u32 offset) {
        u64 value;
        memcpy(&value, m_bytecode.data() + offset, sizeof(u64));
        return value;
    }
};

}

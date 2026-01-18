#include "roxy/compiler/types.hpp"

#include <cstring>

namespace rx {

// Hash function for type interning
u64 TypeHash::operator()(const Type* t) const {
    if (!t) return 0;

    u64 hash = static_cast<u64>(t->kind);

    switch (t->kind) {
        case TypeKind::Array:
            // Hash based on element type pointer
            hash ^= reinterpret_cast<u64>(t->array_info.element_type) * 31;
            break;

        case TypeKind::Function: {
            // Hash based on return type and parameter types
            hash ^= reinterpret_cast<u64>(t->func_info.return_type) * 31;
            for (u32 i = 0; i < t->func_info.param_types.size(); i++) {
                hash ^= reinterpret_cast<u64>(t->func_info.param_types[i]) * (37 + i);
            }
            break;
        }

        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
            // Hash based on inner type pointer
            hash ^= reinterpret_cast<u64>(t->ref_info.inner_type) * 31;
            break;

        case TypeKind::Struct:
        case TypeKind::Enum:
            // Named types use identity - hash by declaration pointer
            hash ^= reinterpret_cast<u64>(t->struct_info.decl) * 31;
            break;

        default:
            // Primitives just use kind
            break;
    }

    return hash;
}

// Equality function for type interning
bool TypeEqual::operator()(const Type* a, const Type* b) const {
    if (a == b) return true;
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;

    switch (a->kind) {
        case TypeKind::Array:
            return a->array_info.element_type == b->array_info.element_type;

        case TypeKind::Function: {
            if (a->func_info.return_type != b->func_info.return_type) return false;
            if (a->func_info.param_types.size() != b->func_info.param_types.size()) return false;
            for (u32 i = 0; i < a->func_info.param_types.size(); i++) {
                if (a->func_info.param_types[i] != b->func_info.param_types[i]) return false;
            }
            return true;
        }

        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
            return a->ref_info.inner_type == b->ref_info.inner_type;

        case TypeKind::Struct:
        case TypeKind::Enum:
            // Named types are equal only if they're the same declaration
            return a->struct_info.decl == b->struct_info.decl;

        default:
            // Primitives are equal if kinds match
            return true;
    }
}

TypeCache::TypeCache(BumpAllocator& allocator)
    : m_allocator(allocator)
{
    // Create primitive type singletons
    m_void = create_primitive(TypeKind::Void);
    m_bool = create_primitive(TypeKind::Bool);
    m_i8 = create_primitive(TypeKind::I8);
    m_i16 = create_primitive(TypeKind::I16);
    m_i32 = create_primitive(TypeKind::I32);
    m_i64 = create_primitive(TypeKind::I64);
    m_u8 = create_primitive(TypeKind::U8);
    m_u16 = create_primitive(TypeKind::U16);
    m_u32 = create_primitive(TypeKind::U32);
    m_u64 = create_primitive(TypeKind::U64);
    m_f32 = create_primitive(TypeKind::F32);
    m_f64 = create_primitive(TypeKind::F64);
    m_string = create_primitive(TypeKind::String);
    m_nil = create_primitive(TypeKind::Nil);
    m_error = create_primitive(TypeKind::Error);
}

Type* TypeCache::create_primitive(TypeKind kind) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = kind;
    return type;
}

Type* TypeCache::intern_type(Type* type) {
    auto it = m_interned.find(type);
    if (it != m_interned.end()) {
        return it->second;
    }
    m_interned[type] = type;
    return type;
}

Type* TypeCache::array_type(Type* element_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Array;
    type->array_info.element_type = element_type;
    return intern_type(type);
}

Type* TypeCache::function_type(Span<Type*> param_types, Type* return_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Function;

    // Copy param types to allocator
    Type** params = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * param_types.size(), alignof(Type*)));
    for (u32 i = 0; i < param_types.size(); i++) {
        params[i] = param_types[i];
    }

    type->func_info.param_types = Span<Type*>(params, param_types.size());
    type->func_info.return_type = return_type;
    return intern_type(type);
}

Type* TypeCache::uniq_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Uniq;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::ref_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Ref;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::weak_type(Type* inner_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Weak;
    type->ref_info.inner_type = inner_type;
    return intern_type(type);
}

Type* TypeCache::struct_type(StringView name, Decl* decl) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Struct;
    type->struct_info.name = name;
    type->struct_info.decl = decl;
    type->struct_info.parent = nullptr;
    type->struct_info.fields = Span<FieldInfo>();
    // Named types are not interned - each declaration creates a unique type
    return type;
}

Type* TypeCache::enum_type(StringView name, Decl* decl, Type* underlying) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Enum;
    type->enum_info.name = name;
    type->enum_info.decl = decl;
    type->enum_info.underlying = underlying ? underlying : m_i32;
    return type;
}

Type* TypeCache::primitive_by_name(StringView name) {
    if (name == "void") return m_void;
    if (name == "bool") return m_bool;
    if (name == "i8") return m_i8;
    if (name == "i16") return m_i16;
    if (name == "i32") return m_i32;
    if (name == "i64") return m_i64;
    if (name == "u8") return m_u8;
    if (name == "u16") return m_u16;
    if (name == "u32") return m_u32;
    if (name == "u64") return m_u64;
    if (name == "f32") return m_f32;
    if (name == "f64") return m_f64;
    if (name == "string") return m_string;
    return nullptr;
}

void TypeCache::register_named_type(StringView name, Type* type) {
    m_named_types[name] = type;
}

Type* TypeCache::named_type_by_name(StringView name) {
    auto it = m_named_types.find(name);
    if (it != m_named_types.end()) {
        return it->second;
    }
    return nullptr;
}

Type* TypeCache::type_by_name(StringView name) {
    // First try primitives
    Type* prim = primitive_by_name(name);
    if (prim) return prim;

    // Then try named types (structs, enums)
    return named_type_by_name(name);
}

const char* type_kind_to_string(TypeKind kind) {
    switch (kind) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::I8: return "i8";
        case TypeKind::I16: return "i16";
        case TypeKind::I32: return "i32";
        case TypeKind::I64: return "i64";
        case TypeKind::U8: return "u8";
        case TypeKind::U16: return "u16";
        case TypeKind::U32: return "u32";
        case TypeKind::U64: return "u64";
        case TypeKind::F32: return "f32";
        case TypeKind::F64: return "f64";
        case TypeKind::String: return "string";
        case TypeKind::Array: return "array";
        case TypeKind::Function: return "function";
        case TypeKind::Struct: return "struct";
        case TypeKind::Enum: return "enum";
        case TypeKind::Uniq: return "uniq";
        case TypeKind::Ref: return "ref";
        case TypeKind::Weak: return "weak";
        case TypeKind::Nil: return "nil";
        case TypeKind::Error: return "<error>";
    }
    return "<unknown>";
}

static void append_string(Vector<char>& out, const char* str) {
    while (*str) {
        out.push_back(*str++);
    }
}

static void append_string(Vector<char>& out, StringView str) {
    for (u32 i = 0; i < str.size(); i++) {
        out.push_back(str[i]);
    }
}

void type_to_string(const Type* type, Vector<char>& out) {
    if (!type) {
        append_string(out, "<null>");
        return;
    }

    switch (type->kind) {
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::I8:
        case TypeKind::I16:
        case TypeKind::I32:
        case TypeKind::I64:
        case TypeKind::U8:
        case TypeKind::U16:
        case TypeKind::U32:
        case TypeKind::U64:
        case TypeKind::F32:
        case TypeKind::F64:
        case TypeKind::String:
        case TypeKind::Nil:
        case TypeKind::Error:
            append_string(out, type_kind_to_string(type->kind));
            break;

        case TypeKind::Array:
            type_to_string(type->array_info.element_type, out);
            append_string(out, "[]");
            break;

        case TypeKind::Function: {
            append_string(out, "fun(");
            for (u32 i = 0; i < type->func_info.param_types.size(); i++) {
                if (i > 0) append_string(out, ", ");
                type_to_string(type->func_info.param_types[i], out);
            }
            append_string(out, "): ");
            type_to_string(type->func_info.return_type, out);
            break;
        }

        case TypeKind::Struct:
            append_string(out, type->struct_info.name);
            break;

        case TypeKind::Enum:
            append_string(out, type->enum_info.name);
            break;

        case TypeKind::Uniq:
            append_string(out, "uniq ");
            type_to_string(type->ref_info.inner_type, out);
            break;

        case TypeKind::Ref:
            append_string(out, "ref ");
            type_to_string(type->ref_info.inner_type, out);
            break;

        case TypeKind::Weak:
            append_string(out, "weak ");
            type_to_string(type->ref_info.inner_type, out);
            break;
    }
}

}

#include "roxy/compiler/types.hpp"

#include <cstring>

namespace rx {

// Hash function for type interning
u64 TypeHash::operator()(const Type* t) const {
    if (!t) return 0;

    u64 hash = static_cast<u64>(t->kind);

    switch (t->kind) {
        case TypeKind::List:
            // Hash based on element type pointer
            hash ^= reinterpret_cast<u64>(t->list_info.element_type) * 31;
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
        case TypeKind::Trait:
            // Named types use identity - hash by declaration pointer
            hash ^= reinterpret_cast<u64>(t->struct_info.decl) * 31;
            break;

        case TypeKind::TypeParam:
            // Hash by name and index
            hash ^= static_cast<u64>(t->type_param_info.index) * 31;
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
        case TypeKind::List:
            return a->list_info.element_type == b->list_info.element_type;

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
        case TypeKind::Trait:
            // Named types are equal only if they're the same declaration
            return a->struct_info.decl == b->struct_info.decl;

        case TypeKind::TypeParam:
            return a->type_param_info.index == b->type_param_info.index &&
                   a->type_param_info.name == b->type_param_info.name;

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

Type* TypeCache::list_type(Type* element_type) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::List;
    type->list_info.element_type = element_type;
    type->list_info.methods = Span<MethodInfo>();
    type->list_info.alloc_native_name = StringView(nullptr, 0);

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

Type* TypeCache::struct_type(StringView name, Decl* decl, StringView module_name) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Struct;
    type->struct_info.name = name;
    // Use empty string "" if module_name is empty to ensure valid StringView
    type->struct_info.module_name = module_name.empty() ? StringView("", 0) : module_name;
    type->struct_info.decl = decl;
    type->struct_info.parent = nullptr;
    type->struct_info.fields = Span<FieldInfo>();
    type->struct_info.implemented_traits = Span<Type*>();
    type->struct_info.slot_count = 0;
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

Type* TypeCache::trait_type(StringView name, Decl* decl) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::Trait;
    type->trait_info.name = name;
    type->trait_info.decl = decl;
    type->trait_info.parent = nullptr;
    type->trait_info.methods = Span<TraitMethodInfo>();
    type->trait_info.type_params = Span<TypeParam>();
    return type;
}

Type* TypeCache::type_param(StringView name, u32 index) {
    Type* type = m_allocator.emplace<Type>();
    type->kind = TypeKind::TypeParam;
    type->type_param_info.name = name;
    type->type_param_info.index = index;
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

const FieldInfo* StructTypeInfo::find_field(StringView field_name) const {
    for (const auto& field : fields) {
        if (field.name == field_name) {
            return &field;
        }
    }
    return nullptr;
}

const VariantFieldInfo* StructTypeInfo::find_variant_field(StringView field_name,
                                                            const WhenClauseInfo** out_clause,
                                                            const VariantInfo** out_variant) const {
    for (const auto& clause : when_clauses) {
        for (const auto& variant : clause.variants) {
            for (const auto& field : variant.fields) {
                if (field.name == field_name) {
                    if (out_clause) *out_clause = &clause;
                    if (out_variant) *out_variant = &variant;
                    return &field;
                }
            }
        }
    }
    return nullptr;
}

void TypeCache::register_primitive_method(TypeKind kind, const MethodInfo& method) {
    m_primitive_methods[static_cast<u8>(kind)].push_back(method);
}

void TypeCache::register_primitive_trait(TypeKind kind, Type* trait) {
    m_primitive_traits[static_cast<u8>(kind)].push_back(trait);
}

const MethodInfo* TypeCache::lookup_primitive_method(TypeKind kind, StringView name) const {
    auto it = m_primitive_methods.find(static_cast<u8>(kind));
    if (it == m_primitive_methods.end()) return nullptr;
    for (const auto& method : it->second) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

bool TypeCache::primitive_implements_trait(TypeKind kind, Type* trait) const {
    auto it = m_primitive_traits.find(static_cast<u8>(kind));
    if (it == m_primitive_traits.end()) return false;
    for (auto* t : it->second) {
        if (t == trait) return true;
    }
    return false;
}

const MethodInfo* TypeCache::lookup_method(Type* type, StringView name, Type** found_in) const {
    if (type->is_struct()) {
        return lookup_method_in_hierarchy(type, name, found_in);
    }
    if (type->is_primitive()) {
        return lookup_primitive_method(type->kind, name);
    }
    return nullptr;
}

bool TypeCache::implements_trait(Type* type, Type* trait) const {
    if (type->is_struct()) {
        StructTypeInfo& sti = type->struct_info;
        for (auto* impl_trait : sti.implemented_traits) {
            if (impl_trait == trait) return true;
        }
        return false;
    }
    if (type->is_primitive()) {
        return primitive_implements_trait(type->kind, trait);
    }
    if (type->is_enum()) {
        // Enums can implement traits through their underlying type (i32)
        return primitive_implements_trait(TypeKind::I32, trait);
    }
    return false;
}

const MethodInfo* lookup_method_in_hierarchy(Type* struct_type, StringView name, Type** found_in_type) {
    Type* current = struct_type;
    while (current && current->is_struct()) {
        StructTypeInfo& sti = current->struct_info;
        for (auto& method : sti.methods) {
            if (method.name == name) {
                if (found_in_type) *found_in_type = current;
                return &method;
            }
        }
        current = sti.parent;
    }
    return nullptr;
}

const MethodInfo* lookup_list_method(const ListTypeInfo& info, StringView name) {
    for (const auto& method : info.methods) {
        if (method.name == name) return &method;
    }
    return nullptr;
}

bool is_subtype_of(Type* child, Type* parent) {
    if (child == parent) return true;
    if (!child || !parent) return false;
    if (!child->is_struct() || !parent->is_struct()) return false;

    Type* current = child->struct_info.parent;
    while (current) {
        if (current == parent) return true;
        current = current->struct_info.parent;
    }
    return false;
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
        case TypeKind::List: return "list";
        case TypeKind::Function: return "function";
        case TypeKind::Struct: return "struct";
        case TypeKind::Enum: return "enum";
        case TypeKind::Trait: return "trait";
        case TypeKind::Uniq: return "uniq";
        case TypeKind::Ref: return "ref";
        case TypeKind::Weak: return "weak";
        case TypeKind::TypeParam: return "<type_param>";
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
    for (char c : str) {
        out.push_back(c);
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

        case TypeKind::List:
            append_string(out, "List<");
            type_to_string(type->list_info.element_type, out);
            append_string(out, ">");
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

        case TypeKind::Trait:
            append_string(out, "trait ");
            append_string(out, type->trait_info.name);
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

        case TypeKind::TypeParam:
            append_string(out, type->type_param_info.name);
            break;
    }
}

}

#include "roxy/compiler/support/mangling.hpp"

#include "roxy/compiler/types/types.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/format.hpp"
#include "roxy/core/vector.hpp"

#include <cassert>
#include <cstring>

namespace rx {

namespace {
// One definition per mangling kind. Both the arena and owned-String forms format
// through these, so the "$$" ABI has exactly one source of truth.
constexpr const char* kMethod      = "{}$${}";
constexpr const char* kCtor        = "{}$$new";
constexpr const char* kCtorNamed   = "{}$$new$${}";
constexpr const char* kDtor        = "{}$$delete";
constexpr const char* kDtorNamed   = "{}$$delete$${}";
constexpr const char* kModuleLocal = "{}::{}";
}  // namespace

StringView mangle_method(BumpAllocator& alloc, StringView struct_name, StringView method_name) {
    return format_to_arena(alloc, runtime(kMethod), struct_name, method_name);
}

String mangle_method_owned(StringView struct_name, StringView method_name) {
    return format(runtime(kMethod), struct_name, method_name);
}

StringView mangle_constructor(BumpAllocator& alloc, StringView struct_name, StringView ctor_name) {
    if (ctor_name.empty()) {
        return format_to_arena(alloc, runtime(kCtor), struct_name);
    }
    return format_to_arena(alloc, runtime(kCtorNamed), struct_name, ctor_name);
}

StringView mangle_destructor(BumpAllocator& alloc, StringView struct_name, StringView dtor_name) {
    if (dtor_name.empty()) {
        return format_to_arena(alloc, runtime(kDtor), struct_name);
    }
    return format_to_arena(alloc, runtime(kDtorNamed), struct_name, dtor_name);
}

String mangle_destructor_owned(StringView struct_name, StringView dtor_name) {
    if (dtor_name.empty()) {
        return format(runtime(kDtor), struct_name);
    }
    return format(runtime(kDtorNamed), struct_name, dtor_name);
}

StringView mangle_module_local(BumpAllocator& alloc, StringView module_name, StringView name) {
    if (module_name.empty()) return name;
    return format_to_arena(alloc, runtime(kModuleLocal), module_name, name);
}

StringView mangle_overload(BumpAllocator& alloc, StringView fun_name, Span<Type*> param_types) {
    // "$ol$" + name + ("$" + type_name)* — see the header comment for the
    // collision-proofing rationale.
    const StringView prefix = "$ol$";
    u32 total_len = prefix.size() + fun_name.size();
    Vector<StringView> param_names;
    for (Type* param_type : param_types) {
        StringView pname = mangle_type_name(alloc, param_type);
        param_names.push_back(pname);
        total_len += 1 + pname.size();  // '$' + name
    }
    char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
    u32 pos = 0;
    memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
    memcpy(buf + pos, fun_name.data(), fun_name.size()); pos += fun_name.size();
    for (auto& pname : param_names) {
        buf[pos++] = '$';
        memcpy(buf + pos, pname.data(), pname.size()); pos += pname.size();
    }
    buf[pos] = '\0';
    return StringView(buf, total_len);
}

StringView mangle_type_name(BumpAllocator& alloc, Type* type) {
    if (!type) return "void";

    switch (type->kind) {
        case TypeKind::Void:   return "void";
        case TypeKind::Bool:   return "bool";
        case TypeKind::I8:     return "i8";
        case TypeKind::I16:    return "i16";
        case TypeKind::I32:    return "i32";
        case TypeKind::I64:    return "i64";
        case TypeKind::U8:     return "u8";
        case TypeKind::U16:    return "u16";
        case TypeKind::U32:    return "u32";
        case TypeKind::U64:    return "u64";
        case TypeKind::F32:    return "f32";
        case TypeKind::F64:    return "f64";
        case TypeKind::String: return "string";
        case TypeKind::Struct: return type->struct_info.name;
        case TypeKind::Enum:   return type->enum_info.name;
        case TypeKind::Trait:  return type->trait_info.name;
        case TypeKind::Nil:    return "nil";
        case TypeKind::Self:   return "Self";
        case TypeKind::IntLiteral: return "i32";
        case TypeKind::FloatLiteral: return "f64";
        case TypeKind::ExceptionRef: return "ExceptionRef";
        case TypeKind::Error:  return "error";
        case TypeKind::TypeParam: {
            // Reserved '$' prefix: a TypeParam argument marks an ABSTRACT
            // instantiation (Phase B checking a bounded template body that
            // names e.g. Box<T>). "Box$$T" cannot collide with any concrete
            // instance — '$' is illegal in user identifiers, so a user struct
            // literally named "T" still mangles to the distinct "Box$T".
            StringView param_name = type->type_param_info.name;
            u32 total_len = 1 + param_name.size();
            char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
            buf[0] = '$';
            memcpy(buf + 1, param_name.data(), param_name.size());
            buf[total_len] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::List: {
            // List$<elem>
            StringView prefix = "List";
            StringView elem = mangle_type_name(alloc, type->list_info.element_type);
            u32 total_len = prefix.size() + 1 + elem.size();
            char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, elem.data(), elem.size()); pos += elem.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Map: {
            // Map$<key>$<value>
            StringView prefix = "Map";
            StringView key_name = mangle_type_name(alloc, type->map_info.key_type);
            StringView val_name = mangle_type_name(alloc, type->map_info.value_type);
            u32 total_len = prefix.size() + 1 + key_name.size() + 1 + val_name.size();
            char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, key_name.data(), key_name.size()); pos += key_name.size();
            buf[pos++] = '$';
            memcpy(buf + pos, val_name.data(), val_name.size()); pos += val_name.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak: {
            // uniq$<inner>, ref$<inner>, weak$<inner>
            StringView prefix;
            if (type->kind == TypeKind::Uniq) prefix = "uniq";
            else if (type->kind == TypeKind::Ref) prefix = "ref";
            else prefix = "weak";
            StringView inner = mangle_type_name(alloc, type->ref_info.inner_type);
            u32 total_len = prefix.size() + 1 + inner.size();
            char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, inner.data(), inner.size()); pos += inner.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Function: {
            // fun$<param1>$<param2>_ret$<return>
            StringView prefix = "fun";
            StringView ret_sep = "_ret";
            StringView ret_name = mangle_type_name(alloc, type->func_info.return_type);
            u32 total_len = prefix.size();
            Vector<StringView> param_names;
            for (auto* param_type : type->func_info.param_types) {
                StringView pname = mangle_type_name(alloc, param_type);
                param_names.push_back(pname);
                total_len += 1 + pname.size(); // '$' + name
            }
            total_len += ret_sep.size() + 1 + ret_name.size(); // _ret$<return>
            char* buf = reinterpret_cast<char*>(alloc.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            for (auto& pname : param_names) {
                buf[pos++] = '$';
                memcpy(buf + pos, pname.data(), pname.size()); pos += pname.size();
            }
            memcpy(buf + pos, ret_sep.data(), ret_sep.size()); pos += ret_sep.size();
            buf[pos++] = '$';
            memcpy(buf + pos, ret_name.data(), ret_name.size()); pos += ret_name.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
    }
    assert(false && "Unhandled type kind in mangle_type_name");
    return "unknown";
}

}  // namespace rx

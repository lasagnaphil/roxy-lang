#include "roxy/vm/binding/registry.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"

#include <cassert>
#include <cstring>

namespace rx {

// File-local helpers

static u32 slot_count_for_kind(NativeTypeKind kind) {
    switch (kind) {
        case NativeTypeKind::I64:
        case NativeTypeKind::U64:
        case NativeTypeKind::F64:
            return 2;
        default:
            return 1;
    }
}

static Type* type_from_kind(NativeTypeKind kind, TypeCache& types) {
    switch (kind) {
        case NativeTypeKind::Void: return types.void_type();
        case NativeTypeKind::Bool: return types.bool_type();
        case NativeTypeKind::I8: return types.i8_type();
        case NativeTypeKind::I16: return types.i16_type();
        case NativeTypeKind::I32: return types.i32_type();
        case NativeTypeKind::I64: return types.i64_type();
        case NativeTypeKind::U8: return types.u8_type();
        case NativeTypeKind::U16: return types.u16_type();
        case NativeTypeKind::U32: return types.u32_type();
        case NativeTypeKind::U64: return types.u64_type();
        case NativeTypeKind::F32: return types.f32_type();
        case NativeTypeKind::F64: return types.f64_type();
        case NativeTypeKind::String: return types.string_type();
        default: return types.error_type();
    }
}

// NativeFunctionEntry type resolution methods

Type* NativeFunctionEntry::resolve_return_type(TypeCache& types) const {
    switch (type_info_mode) {
        case NativeTypeInfoMode::Resolver:
            return return_resolver(types);
        case NativeTypeInfoMode::Parsed:
            return NativeRegistry::resolve_type_expr(return_type_expr, {}, {}, types);
        default:
            return types.error_type();
    }
}

void NativeFunctionEntry::resolve_param_types(TypeCache& types, Type** out_params) const {
    for (u32 i = 0; i < param_count; i++) {
        switch (type_info_mode) {
            case NativeTypeInfoMode::Resolver:
                out_params[i] = param_resolvers[i](types);
                break;
            case NativeTypeInfoMode::Parsed:
                out_params[i] = NativeRegistry::resolve_type_expr(
                    param_type_exprs[i], {}, {}, types);
                break;
        }
    }
}

// NativeRegistry: signature parsing

Decl* NativeRegistry::parse_signature(const char* signature) {
    // Build "native <signature>;" in the allocator so AST StringViews remain valid
    u32 sig_len = static_cast<u32>(strlen(signature));
    u32 total_len = 7 + sig_len + 1; // "native " + sig + ";"
    char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
    memcpy(buf, "native ", 7);
    memcpy(buf + 7, signature, sig_len);
    buf[7 + sig_len] = ';';
    buf[total_len] = '\0';

    Lexer lexer(buf, total_len);
    Parser parser(lexer, m_allocator);
    Program* program = parser.parse();
    assert(!parser.has_error() && "Failed to parse native signature");
    assert(program->declarations.size() == 1);
    return program->declarations[0];
}

void NativeRegistry::parse_type_decl(const char* type_decl, StringView& out_name,
                                     Vector<StringView>& out_param_names) {
    // Find '<' to split "List<T>" → "List", ["T"]
    const char* angle = strchr(type_decl, '<');
    if (!angle) {
        // No type params — just a name
        out_name = make_string_view(type_decl);
        return;
    }

    u32 name_len = static_cast<u32>(angle - type_decl);
    // Copy name into allocator
    char* name_buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(name_len + 1, 1));
    memcpy(name_buf, type_decl, name_len);
    name_buf[name_len] = '\0';
    out_name = StringView(name_buf, name_len);

    // Parse comma-separated identifiers between < and >
    const char* p = angle + 1;
    while (*p && *p != '>') {
        // Skip whitespace
        while (*p == ' ' || *p == ',') p++;
        if (*p == '>' || *p == '\0') break;

        const char* start = p;
        while (*p && *p != ',' && *p != '>' && *p != ' ') p++;
        u32 param_len = static_cast<u32>(p - start);
        char* param_buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(param_len + 1, 1));
        memcpy(param_buf, start, param_len);
        param_buf[param_len] = '\0';
        out_param_names.push_back(StringView(param_buf, param_len));
    }
}

Type* NativeRegistry::resolve_type_expr(TypeExpr* expr,
                                        Span<StringView> type_param_names,
                                        Span<Type*> type_args,
                                        TypeCache& types) {
    if (!expr) return types.void_type();

    StringView name = expr->name;

    // Check if it's a type parameter name
    for (u32 i = 0; i < type_param_names.size(); i++) {
        if (name == type_param_names[i]) {
            if (i < type_args.size()) {
                return type_args[i];
            }
            // No concrete arg — return a type param placeholder
            return types.type_param(name, i);
        }
    }

    // Primitive types
    Type* primitive = types.primitive_by_name(name);
    if (primitive) return primitive;

    // Generic type application: List<T>, Map<K, V>
    if (expr->type_args.size() > 0) {
        // Resolve type args recursively
        if (name == StringView("List", 4) && expr->type_args.size() == 1) {
            Type* elem = resolve_type_expr(expr->type_args[0], type_param_names, type_args, types);
            return types.list_type(elem);
        }
        if (name == StringView("Map", 3) && expr->type_args.size() == 2) {
            Type* key = resolve_type_expr(expr->type_args[0], type_param_names, type_args, types);
            Type* val = resolve_type_expr(expr->type_args[1], type_param_names, type_args, types);
            return types.map_type(key, val);
        }
    }

    return types.error_type();
}

// NativeRegistry method implementations

void NativeRegistry::bind_native(NativeFunction func, const char* signature) {
    Decl* decl = parse_signature(signature);
    assert(decl->kind == AstKind::DeclFun);
    FunDecl& fun = decl->fun_decl;

    NativeFunctionEntry entry;
    entry.name = fun.name;
    entry.func = func;
    entry.type_info_mode = NativeTypeInfoMode::Parsed;
    entry.param_count = fun.params.size();
    entry.min_args = entry.param_count;
    entry.is_method = false;
    entry.method_kind = GenericMethodKind::Method;
    entry.return_type_expr = fun.return_type;

    // Store param TypeExprs
    TypeExpr** param_exprs = nullptr;
    if (entry.param_count > 0) {
        param_exprs = reinterpret_cast<TypeExpr**>(
            m_allocator.alloc_bytes(sizeof(TypeExpr*) * entry.param_count, alignof(TypeExpr*)));
        for (u32 i = 0; i < entry.param_count; i++) {
            param_exprs[i] = fun.params[i].type;
        }
    }
    entry.param_type_exprs = Span<TypeExpr*>(param_exprs, entry.param_count);

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_native(const char* override_name, NativeFunction func,
                                 const char* signature) {
    Decl* decl = parse_signature(signature);
    assert(decl->kind == AstKind::DeclFun);
    FunDecl& fun = decl->fun_decl;

    NativeFunctionEntry entry;
    entry.name = make_string_view(override_name);
    entry.func = func;
    entry.type_info_mode = NativeTypeInfoMode::Parsed;
    entry.param_count = fun.params.size();
    entry.min_args = entry.param_count;
    entry.is_method = false;
    entry.method_kind = GenericMethodKind::Method;
    entry.return_type_expr = fun.return_type;

    TypeExpr** param_exprs = nullptr;
    if (entry.param_count > 0) {
        param_exprs = reinterpret_cast<TypeExpr**>(
            m_allocator.alloc_bytes(sizeof(TypeExpr*) * entry.param_count, alignof(TypeExpr*)));
        for (u32 i = 0; i < entry.param_count; i++) {
            param_exprs[i] = fun.params[i].type;
        }
    }
    entry.param_type_exprs = Span<TypeExpr*>(param_exprs, entry.param_count);

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::register_struct(const char* name, std::initializer_list<NativeFieldEntry> fields) {
    NativeStructEntry entry;
    entry.name = make_string_view(name);
    for (const auto& f : fields) {
        entry.fields.push_back(f);
    }
    m_struct_entries.push_back(entry);
}

void NativeRegistry::bind_method(NativeFunction func, const char* signature) {
    Decl* decl = parse_signature(signature);
    assert(decl->kind == AstKind::DeclMethod);
    MethodDecl& method = decl->method_decl;

    NativeFunctionEntry entry;
    entry.struct_name = method.struct_name;
    entry.method_name = method.name;
    entry.name = mangle_method_name(entry.struct_name, entry.method_name);
    entry.func = func;
    entry.type_info_mode = NativeTypeInfoMode::Parsed;
    entry.param_count = method.params.size();
    entry.min_args = entry.param_count;
    entry.is_method = true;
    entry.method_kind = GenericMethodKind::Method;
    entry.return_type_expr = method.return_type;

    TypeExpr** param_exprs = nullptr;
    if (entry.param_count > 0) {
        param_exprs = reinterpret_cast<TypeExpr**>(
            m_allocator.alloc_bytes(sizeof(TypeExpr*) * entry.param_count, alignof(TypeExpr*)));
        for (u32 i = 0; i < entry.param_count; i++) {
            param_exprs[i] = method.params[i].type;
        }
    }
    entry.param_type_exprs = Span<TypeExpr*>(param_exprs, entry.param_count);

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_constructor(NativeFunction func, const char* signature, u32 min_args) {
    Decl* decl = parse_signature(signature);
    assert(decl->kind == AstKind::DeclMethod);
    MethodDecl& method = decl->method_decl;

    NativeFunctionEntry entry;
    entry.struct_name = method.struct_name;
    entry.method_name = method.name;
    entry.name = mangle_method_name(entry.struct_name, entry.method_name);
    entry.func = func;
    entry.type_info_mode = NativeTypeInfoMode::Parsed;
    entry.param_count = method.params.size();
    entry.min_args = min_args;
    entry.is_method = true;
    entry.method_kind = GenericMethodKind::Constructor;
    entry.return_type_expr = nullptr; // constructors return void

    TypeExpr** param_exprs = nullptr;
    if (entry.param_count > 0) {
        param_exprs = reinterpret_cast<TypeExpr**>(
            m_allocator.alloc_bytes(sizeof(TypeExpr*) * entry.param_count, alignof(TypeExpr*)));
        for (u32 i = 0; i < entry.param_count; i++) {
            param_exprs[i] = method.params[i].type;
        }
    }
    entry.param_type_exprs = Span<TypeExpr*>(param_exprs, entry.param_count);

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::register_generic_type(const char* type_decl,
                                           const char* alloc_name, NativeFunction alloc_func) {
    NativeGenericTypeEntry entry;
    parse_type_decl(type_decl, entry.name, entry.type_param_names);
    entry.type_param_count = static_cast<u32>(entry.type_param_names.size());
    entry.alloc_native_name = make_string_view(alloc_name);

    m_generic_types[entry.name] = std::move(entry);

    // Register the alloc function as a regular non-method native
    // Alloc functions take no params and return i64 (pointer)
    bind_native(alloc_name, alloc_func, "fun alloc(): i64");
}

void NativeRegistry::bind_generic_destructor(const char* type_name, NativeFunction func) {
    StringView sn = make_string_view(type_name);
    StringView mn = make_string_view("delete");
    StringView mangled = mangle_method_name(sn, mn);

    NativeFunctionEntry entry;
    entry.name = mangled;
    entry.func = func;
    entry.param_count = 0;
    entry.min_args = 0;
    entry.type_info_mode = NativeTypeInfoMode::Parsed;
    entry.is_method = true;
    entry.method_kind = GenericMethodKind::Destructor;
    entry.struct_name = sn;
    entry.method_name = mn;
    entry.return_type_expr = nullptr;

    m_function_entries.push_back(entry);
    m_name_to_index[mangled] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_generic_copy_constructor(const char* type_name,
                                                   const char* copy_func_name,
                                                   NativeFunction func) {
    auto it = m_generic_types.find(make_string_view(type_name));
    if (it == m_generic_types.end()) return;

    StringView copy_name = make_string_view(copy_func_name);
    it.value().copy_native_name = copy_name;

    // Register as a regular native function (takes 1 pointer arg, returns pointer)
    bind_native(copy_func_name, func, "fun copy(src: i64): i64");
}

bool NativeRegistry::has_generic_type(StringView name) const {
    return m_generic_types.find(name) != m_generic_types.end();
}

StringView NativeRegistry::get_generic_alloc_name(StringView name) const {
    auto it = m_generic_types.find(name);
    if (it != m_generic_types.end()) {
        return it->second.alloc_native_name;
    }
    return StringView(nullptr, 0);
}

StringView NativeRegistry::get_generic_copy_name(StringView name) const {
    auto it = m_generic_types.find(name);
    if (it != m_generic_types.end()) {
        return it->second.copy_native_name;
    }
    return StringView(nullptr, 0);
}

Span<MethodInfo> NativeRegistry::instantiate_generic_methods(StringView name, Span<Type*> type_args,
                                                              BumpAllocator& allocator, TypeCache& types) const {
    // Look up type param names for this generic type
    auto gen_it = m_generic_types.find(name);
    Span<StringView> type_param_names;
    if (gen_it != m_generic_types.end()) {
        const auto& param_names = gen_it->second.type_param_names;
        type_param_names = Span<StringView>(
            const_cast<StringView*>(param_names.data()),
            static_cast<u32>(param_names.size()));
    }

    // Count method entries for this struct
    u32 count = 0;
    for (const auto& entry : m_function_entries) {
        if (entry.is_method && entry.struct_name == name &&
            entry.method_kind == GenericMethodKind::Method) {
            count++;
        }
    }
    if (count == 0) return Span<MethodInfo>();

    MethodInfo* methods = reinterpret_cast<MethodInfo*>(
        allocator.alloc_bytes(sizeof(MethodInfo) * count, alignof(MethodInfo)));

    u32 idx = 0;
    for (const auto& entry : m_function_entries) {
        if (!entry.is_method || entry.struct_name != name ||
            entry.method_kind != GenericMethodKind::Method) continue;

        MethodInfo& method_info = methods[idx++];
        method_info.name = entry.method_name;
        method_info.native_name = entry.name;
        method_info.decl = nullptr;

        u32 param_count = entry.param_count;
        Type** param_types = nullptr;
        if (param_count > 0) {
            param_types = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * param_count, alignof(Type*)));
            for (u32 j = 0; j < param_count; j++) {
                switch (entry.type_info_mode) {
                    case NativeTypeInfoMode::Parsed:
                        param_types[j] = resolve_type_expr(
                            entry.param_type_exprs[j], type_param_names, type_args, types);
                        break;
                    case NativeTypeInfoMode::Resolver:
                        param_types[j] = entry.param_resolvers[j](types);
                        break;
                }
            }
        }
        method_info.param_types = Span<Type*>(param_types, param_count);

        switch (entry.type_info_mode) {
            case NativeTypeInfoMode::Parsed:
                method_info.return_type = resolve_type_expr(
                    entry.return_type_expr, type_param_names, type_args, types);
                break;
            case NativeTypeInfoMode::Resolver:
                method_info.return_type = entry.return_resolver(types);
                break;
        }
    }

    return Span<MethodInfo>(methods, count);
}

ResolvedConstructor NativeRegistry::instantiate_generic_constructor(StringView name, Span<Type*> type_args,
                                                                     BumpAllocator& allocator,
                                                                     TypeCache& types) const {
    // Look up type param names for this generic type
    auto gen_it = m_generic_types.find(name);
    Span<StringView> type_param_names;
    if (gen_it != m_generic_types.end()) {
        const auto& param_names = gen_it->second.type_param_names;
        type_param_names = Span<StringView>(
            const_cast<StringView*>(param_names.data()),
            static_cast<u32>(param_names.size()));
    }

    for (const auto& entry : m_function_entries) {
        if (!entry.is_method || entry.struct_name != name ||
            entry.method_kind != GenericMethodKind::Constructor) continue;

        u32 param_count = entry.param_count;
        Type** param_types = nullptr;
        if (param_count > 0) {
            param_types = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * param_count, alignof(Type*)));
            for (u32 j = 0; j < param_count; j++) {
                switch (entry.type_info_mode) {
                    case NativeTypeInfoMode::Parsed:
                        param_types[j] = resolve_type_expr(
                            entry.param_type_exprs[j], type_param_names, type_args, types);
                        break;
                    case NativeTypeInfoMode::Resolver:
                        param_types[j] = entry.param_resolvers[j](types);
                        break;
                }
            }
        }
        return {entry.name, Span<Type*>(param_types, param_count), entry.min_args};
    }

    return {StringView(nullptr, 0), Span<Type*>(), 0};
}

void NativeRegistry::apply_structs_to_types(TypeEnv& type_env, BumpAllocator& allocator, SymbolTable& symbols) {
    TypeCache& types = type_env.types();
    for (const auto& se : m_struct_entries) {
        // Create struct type (decl = nullptr for native structs)
        Type* type = types.struct_type(se.name, nullptr);
        type_env.register_named_type(se.name, type);

        // Build fields
        u32 num_fields = static_cast<u32>(se.fields.size());
        FieldInfo* fields = nullptr;
        if (num_fields > 0) {
            fields = reinterpret_cast<FieldInfo*>(
                allocator.alloc_bytes(sizeof(FieldInfo) * num_fields, alignof(FieldInfo)));
        }

        u32 slot_offset = 0;
        for (u32 i = 0; i < num_fields; i++) {
            Type* field_type = type_from_kind(se.fields[i].type_kind, types);
            u32 sc = slot_count_for_kind(se.fields[i].type_kind);

            u32 name_len = static_cast<u32>(strlen(se.fields[i].name));
            fields[i].name = StringView(se.fields[i].name, name_len);
            fields[i].type = field_type;
            fields[i].is_pub = true;
            fields[i].index = i;
            fields[i].slot_offset = slot_offset;
            fields[i].slot_count = sc;
            slot_offset += sc;
        }

        type->struct_info.fields = Span<FieldInfo>(fields, num_fields);
        type->struct_info.slot_count = slot_offset;

        // Define in symbol table
        symbols.define(SymbolKind::Struct, se.name, type, SourceLocation{0, 0}, nullptr);
    }
}

void NativeRegistry::apply_methods_to_types(TypeEnv& type_env, BumpAllocator& allocator) {
    TypeCache& types = type_env.types();
    // Group methods by struct name
    tsl::robin_map<StringView, Vector<const NativeFunctionEntry*>> methods_by_struct;
    for (const auto& entry : m_function_entries) {
        if (!entry.is_method) continue;
        methods_by_struct[entry.struct_name].push_back(&entry);
    }

    for (auto& [struct_name, methods] : methods_by_struct) {
        Type* struct_type = type_env.named_type_by_name(struct_name);
        if (!struct_type || !struct_type->is_struct()) continue;

        u32 existing = struct_type->struct_info.methods.size();
        u32 total = existing + static_cast<u32>(methods.size());
        MethodInfo* method_info_array = reinterpret_cast<MethodInfo*>(
            allocator.alloc_bytes(sizeof(MethodInfo) * total, alignof(MethodInfo)));

        // Copy existing methods
        for (u32 i = 0; i < existing; i++) {
            method_info_array[i] = struct_type->struct_info.methods[i];
        }

        // Add new native methods
        for (u32 i = 0; i < methods.size(); i++) {
            const NativeFunctionEntry* e = methods[i];
            MethodInfo& method_info = method_info_array[existing + i];
            method_info.name = e->method_name;
            method_info.decl = nullptr;  // Native methods have no AST

            // Build param types (excluding self)
            u32 param_count = e->param_count;
            Type** param_types = nullptr;
            if (param_count > 0) {
                param_types = reinterpret_cast<Type**>(
                    allocator.alloc_bytes(sizeof(Type*) * param_count, alignof(Type*)));
                e->resolve_param_types(types, param_types);
            }
            method_info.param_types = Span<Type*>(param_types, param_count);
            method_info.return_type = e->resolve_return_type(types);
        }

        struct_type->struct_info.methods = Span<MethodInfo>(method_info_array, total);
    }
}

void NativeRegistry::apply_to_symbols(SymbolTable& symbols, TypeCache& types, BumpAllocator& allocator) {
    for (const auto& entry : m_function_entries) {
        // Skip method entries - they are applied via apply_methods_to_types
        if (entry.is_method) continue;

        // Get return type
        Type* ret_type = entry.resolve_return_type(types);

        // Allocate and resolve param types
        Type** param_array = nullptr;
        if (entry.param_count > 0) {
            param_array = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * entry.param_count, alignof(Type*)));
            entry.resolve_param_types(types, param_array);
        }

        // Create function type using provided TypeCache
        Type* func_type = types.function_type(
            Span<Type*>(param_array, entry.param_count), ret_type);

        // Define in symbol table
        symbols.define(SymbolKind::Function, entry.name, func_type,
                      SourceLocation{0, 0}, nullptr);
    }
}

void NativeRegistry::apply_to_symbols(SymbolTable& symbols) {
    apply_to_symbols(symbols, m_types, m_allocator);
}

void NativeRegistry::apply_to_module(BCModule* module) {
    for (const auto& entry : m_function_entries) {
        BCNativeFunction bc_native;
        bc_native.name = entry.name;
        bc_native.func = entry.func;
        // For methods, param_count excludes self but the native function receives self
        bc_native.param_count = entry.is_method ? entry.param_count + 1 : entry.param_count;
        module->native_functions.push_back(bc_native);
    }
}

i32 NativeRegistry::get_index(StringView name) const {
    auto it = m_name_to_index.find(name);
    if (it != m_name_to_index.end()) {
        return it->second;
    }
    return -1;
}

bool NativeRegistry::is_native(StringView name) const {
    return m_name_to_index.find(name) != m_name_to_index.end();
}

void NativeRegistry::copy_entries_to(NativeRegistry& other) const {
    for (const auto& entry : m_function_entries) {
        other.m_function_entries.push_back(entry);
        other.m_name_to_index[entry.name] = static_cast<i32>(other.m_function_entries.size() - 1);
    }
    // Copy struct entries
    for (const auto& se : m_struct_entries) {
        other.m_struct_entries.push_back(se);
    }
    // Copy generic type entries
    for (const auto& [name, entry] : m_generic_types) {
        other.m_generic_types[name] = entry;
    }
}

} // namespace rx

#include "roxy/vm/binding/registry.hpp"
#include "roxy/compiler/type_env.hpp"

#include <cstring>

namespace rx {

// File-local helpers (were private statics on NativeRegistry)

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

static Type* resolve_param_desc(const NativeParamDesc& desc, Span<Type*> type_args, TypeCache& types) {
    if (desc.is_type_param) {
        if (desc.type_param_index < type_args.size()) {
            return type_args[desc.type_param_index];
        }
        return types.error_type();
    }
    return type_from_kind(desc.kind, types);
}

// NativeRegistry method implementations

void NativeRegistry::bind_manual(const char* name, NativeFunction func,
                                 std::initializer_list<Type*> param_types, Type* return_type) {
    NativeFunctionEntry entry;
    entry.name = make_string_view(name);
    entry.func = func;
    entry.return_type = return_type;
    entry.return_desc = concrete_param(NativeTypeKind::Void);  // Not used for manual
    entry.param_count = static_cast<u32>(param_types.size());
    entry.min_args = entry.param_count;
    entry.is_manual = true;
    entry.is_method = false;
    entry.method_kind = GenericMethodKind::Method;

    for (Type* t : param_types) {
        entry.param_types.push_back(t);
    }

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_native(const char* name, NativeFunction func,
                                 std::initializer_list<NativeTypeKind> param_type_kinds,
                                 NativeTypeKind return_type_kind) {
    NativeFunctionEntry entry;
    entry.name = make_string_view(name);
    entry.func = func;
    entry.return_type = nullptr;
    entry.return_desc = concrete_param(return_type_kind);
    entry.param_count = static_cast<u32>(param_type_kinds.size());
    entry.min_args = entry.param_count;
    entry.is_manual = false;  // Use type kinds, not stored Type*
    entry.is_method = false;
    entry.method_kind = GenericMethodKind::Method;

    for (NativeTypeKind k : param_type_kinds) {
        entry.param_descs.push_back(concrete_param(k));
    }

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

void NativeRegistry::bind_method_native(const char* struct_name, const char* method_name,
                                        NativeFunction func,
                                        std::initializer_list<NativeTypeKind> param_type_kinds,
                                        NativeTypeKind return_type_kind) {
    NativeFunctionEntry entry;
    entry.struct_name = make_string_view(struct_name);
    entry.method_name = make_string_view(method_name);
    entry.name = mangle_method_name(entry.struct_name, entry.method_name);
    entry.func = func;
    entry.return_type = nullptr;
    entry.return_desc = concrete_param(return_type_kind);
    entry.param_count = static_cast<u32>(param_type_kinds.size());
    entry.min_args = entry.param_count;
    entry.is_manual = false;
    entry.is_method = true;
    entry.method_kind = GenericMethodKind::Method;

    for (NativeTypeKind k : param_type_kinds) {
        entry.param_descs.push_back(concrete_param(k));
    }

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_method_manual(const char* struct_name, const char* method_name,
                                        NativeFunction func,
                                        std::initializer_list<Type*> param_types, Type* return_type) {
    NativeFunctionEntry entry;
    entry.struct_name = make_string_view(struct_name);
    entry.method_name = make_string_view(method_name);
    entry.name = mangle_method_name(entry.struct_name, entry.method_name);
    entry.func = func;
    entry.return_type = return_type;
    entry.return_desc = concrete_param(NativeTypeKind::Void);
    entry.param_count = static_cast<u32>(param_types.size());
    entry.min_args = entry.param_count;
    entry.is_manual = true;
    entry.is_method = true;
    entry.method_kind = GenericMethodKind::Method;

    for (Type* t : param_types) {
        entry.param_types.push_back(t);
    }

    m_function_entries.push_back(entry);
    m_name_to_index[entry.name] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::register_generic_type(const char* name, u32 type_param_count,
                                           const char* alloc_name, NativeFunction alloc_func) {
    NativeGenericTypeEntry entry;
    entry.name = make_string_view(name);
    entry.type_param_count = type_param_count;
    entry.alloc_native_name = make_string_view(alloc_name);

    m_generic_types[entry.name] = std::move(entry);

    // Register the alloc function as a regular non-method native
    bind_native(alloc_name, alloc_func, {}, NativeTypeKind::I64);
}

void NativeRegistry::bind_generic_method(const char* type_name, const char* method_name,
                                         NativeFunction func,
                                         std::initializer_list<NativeParamDesc> params,
                                         NativeParamDesc return_desc) {
    StringView sn = make_string_view(type_name);
    StringView mn = make_string_view(method_name);
    StringView mangled = mangle_method_name(sn, mn);

    NativeFunctionEntry ne;
    ne.name = mangled;
    ne.func = func;
    ne.param_count = static_cast<u32>(params.size());
    ne.min_args = ne.param_count;
    ne.is_manual = false;
    ne.is_method = true;
    ne.method_kind = GenericMethodKind::Method;
    ne.struct_name = sn;
    ne.method_name = mn;
    ne.return_desc = return_desc;
    ne.return_type = nullptr;
    for (auto& p : params) ne.param_descs.push_back(p);

    m_function_entries.push_back(ne);
    m_name_to_index[mangled] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_generic_constructor(const char* type_name, NativeFunction func,
                                              u32 min_args,
                                              std::initializer_list<NativeParamDesc> params) {
    StringView sn = make_string_view(type_name);
    StringView mn = make_string_view("new");
    StringView mangled = mangle_method_name(sn, mn);

    NativeFunctionEntry ne;
    ne.name = mangled;
    ne.func = func;
    ne.param_count = static_cast<u32>(params.size());
    ne.min_args = min_args;
    ne.is_manual = false;
    ne.is_method = true;
    ne.method_kind = GenericMethodKind::Constructor;
    ne.struct_name = sn;
    ne.method_name = mn;
    ne.return_desc = concrete_param(NativeTypeKind::Void);
    ne.return_type = nullptr;
    for (auto& p : params) ne.param_descs.push_back(p);

    m_function_entries.push_back(ne);
    m_name_to_index[mangled] = static_cast<i32>(m_function_entries.size() - 1);
}

void NativeRegistry::bind_generic_destructor(const char* type_name, NativeFunction func) {
    StringView sn = make_string_view(type_name);
    StringView mn = make_string_view("delete");
    StringView mangled = mangle_method_name(sn, mn);

    NativeFunctionEntry ne;
    ne.name = mangled;
    ne.func = func;
    ne.param_count = 0;
    ne.min_args = 0;
    ne.is_manual = false;
    ne.is_method = true;
    ne.method_kind = GenericMethodKind::Destructor;
    ne.struct_name = sn;
    ne.method_name = mn;
    ne.return_desc = concrete_param(NativeTypeKind::Void);
    ne.return_type = nullptr;

    m_function_entries.push_back(ne);
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
    bind_native(copy_func_name, func, {NativeTypeKind::I64}, NativeTypeKind::I64);
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

        MethodInfo& mi = methods[idx++];
        mi.name = entry.method_name;
        mi.native_name = entry.name;
        mi.decl = nullptr;

        u32 pc = entry.param_count;
        Type** ptypes = nullptr;
        if (pc > 0) {
            ptypes = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * pc, alignof(Type*)));
            for (u32 j = 0; j < pc; j++) {
                ptypes[j] = resolve_param_desc(entry.param_descs[j], type_args, types);
            }
        }
        mi.param_types = Span<Type*>(ptypes, pc);
        mi.return_type = resolve_param_desc(entry.return_desc, type_args, types);
    }

    return Span<MethodInfo>(methods, count);
}

ResolvedConstructor NativeRegistry::instantiate_generic_constructor(StringView name, Span<Type*> type_args,
                                                                     BumpAllocator& allocator,
                                                                     TypeCache& types) const {
    for (const auto& entry : m_function_entries) {
        if (!entry.is_method || entry.struct_name != name ||
            entry.method_kind != GenericMethodKind::Constructor) continue;

        u32 pc = entry.param_count;
        Type** ptypes = nullptr;
        if (pc > 0) {
            ptypes = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * pc, alignof(Type*)));
            for (u32 j = 0; j < pc; j++) {
                ptypes[j] = resolve_param_desc(entry.param_descs[j], type_args, types);
            }
        }
        return {entry.name, Span<Type*>(ptypes, pc), entry.min_args};
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
        MethodInfo* mi_array = reinterpret_cast<MethodInfo*>(
            allocator.alloc_bytes(sizeof(MethodInfo) * total, alignof(MethodInfo)));

        // Copy existing methods
        for (u32 i = 0; i < existing; i++) {
            mi_array[i] = struct_type->struct_info.methods[i];
        }

        // Add new native methods
        for (u32 i = 0; i < methods.size(); i++) {
            const NativeFunctionEntry* e = methods[i];
            MethodInfo& mi = mi_array[existing + i];
            mi.name = e->method_name;
            mi.decl = nullptr;  // Native methods have no AST

            // Build param types (excluding self)
            u32 pc = e->param_count;
            Type** ptypes = nullptr;
            if (pc > 0) {
                ptypes = reinterpret_cast<Type**>(
                    allocator.alloc_bytes(sizeof(Type*) * pc, alignof(Type*)));
                for (u32 j = 0; j < pc; j++) {
                    if (e->is_manual) {
                        ptypes[j] = e->param_types[j];
                    } else {
                        ptypes[j] = type_from_kind(e->param_descs[j].kind, types);
                    }
                }
            }
            mi.param_types = Span<Type*>(ptypes, pc);

            if (e->is_manual) {
                mi.return_type = e->return_type;
            } else {
                mi.return_type = type_from_kind(e->return_desc.kind, types);
            }
        }

        struct_type->struct_info.methods = Span<MethodInfo>(mi_array, total);
    }
}

void NativeRegistry::apply_to_symbols(SymbolTable& symbols, TypeCache& types, BumpAllocator& allocator) {
    for (const auto& entry : m_function_entries) {
        // Skip method entries - they are applied via apply_methods_to_types
        if (entry.is_method) continue;

        // Get or create return type
        Type* ret_type = entry.is_manual ? entry.return_type
                                          : type_from_kind(entry.return_desc.kind, types);

        // Allocate param types array
        Type** param_array = nullptr;
        if (entry.param_count > 0) {
            param_array = reinterpret_cast<Type**>(
                allocator.alloc_bytes(sizeof(Type*) * entry.param_count, alignof(Type*)));
            for (u32 i = 0; i < entry.param_count; i++) {
                if (entry.is_manual) {
                    param_array[i] = entry.param_types[i];
                } else {
                    param_array[i] = type_from_kind(entry.param_descs[i].kind, types);
                }
            }
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

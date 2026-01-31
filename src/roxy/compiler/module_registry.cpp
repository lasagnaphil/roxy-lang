#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/binding/registry.hpp"

namespace rx {

void ModuleRegistry::register_native_module(StringView name, NativeRegistry* natives, TypeCache& types) {
    // Create module info
    ModuleInfo* module = m_allocator.emplace<ModuleInfo>();
    module->name = name;
    module->natives = natives;
    module->is_native = true;

    // Add all native functions as exports
    for (u32 i = 0; i < natives->size(); i++) {
        const NativeEntry& entry = natives->get_entry(i);

        ModuleExport exp;
        exp.name = entry.name;
        exp.kind = ExportKind::Function;
        exp.is_native = true;
        exp.is_pub = true;  // All native functions are public
        exp.index = i;
        exp.decl = nullptr;

        // Get the function type from the native entry
        // We need to build the function type from the native entry's type info
        if (entry.is_manual) {
            // Manual binding - types are stored directly
            Type** param_array = nullptr;
            if (entry.param_count > 0) {
                param_array = reinterpret_cast<Type**>(
                    m_allocator.alloc_bytes(sizeof(Type*) * entry.param_count, alignof(Type*)));
                for (u32 j = 0; j < entry.param_count; j++) {
                    param_array[j] = entry.param_types[j];
                }
            }
            exp.type = types.function_type(Span<Type*>(param_array, entry.param_count), entry.return_type);
        } else {
            // Automatic binding - need to convert type kinds to types
            Type** param_array = nullptr;
            if (entry.param_count > 0) {
                param_array = reinterpret_cast<Type**>(
                    m_allocator.alloc_bytes(sizeof(Type*) * entry.param_count, alignof(Type*)));
                for (u32 j = 0; j < entry.param_count; j++) {
                    param_array[j] = type_from_kind(entry.param_type_kinds[j], types);
                }
            }
            Type* ret_type = type_from_kind(entry.return_type_kind, types);
            exp.type = types.function_type(Span<Type*>(param_array, entry.param_count), ret_type);
        }

        module->exports.push_back(exp);
    }

    m_modules[name] = module;
}

ModuleInfo* ModuleRegistry::register_script_module(StringView name) {
    ModuleInfo* module = m_allocator.emplace<ModuleInfo>();
    module->name = name;
    module->natives = nullptr;
    module->is_native = false;

    m_modules[name] = module;
    return module;
}

void ModuleRegistry::add_export(ModuleInfo* module, StringView name, ExportKind kind,
                                Type* type, bool is_pub, u32 index, Decl* decl) {
    ModuleExport exp;
    exp.name = name;
    exp.kind = kind;
    exp.type = type;
    exp.is_native = false;
    exp.is_pub = is_pub;
    exp.index = index;
    exp.decl = decl;

    module->exports.push_back(exp);
}

// Helper function to convert NativeTypeKind to Type*
Type* type_from_kind(NativeTypeKind kind, TypeCache& types) {
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
        case NativeTypeKind::ArrayI32: return types.array_type(types.i32_type());
        default: return types.error_type();
    }
}

}

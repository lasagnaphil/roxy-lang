#include "roxy/lsp/global_index.hpp"

namespace rx {

String GlobalIndex::make_qualified_key(StringView struct_name, StringView member_name) {
    String key;
    key.reserve(struct_name.size() + 1 + member_name.size());
    key.append(struct_name);
    key.push_back('.');
    key.append(member_name);
    return key;
}

void GlobalIndex::update_file(const String& uri, const FileStubs& stubs) {
    // Remove old entries first
    remove_file(StringView(uri.data(), uri.size()));

    FileNameSet name_set;

    // Index structs
    for (u32 i = 0; i < stubs.structs.size(); i++) {
        const StructStub& stub = stubs.structs[i];
        String name(stub.name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_structs[name] = location;
        name_set.struct_names.push_back(name);

        // Index fields
        for (u32 j = 0; j < stub.fields.size(); j++) {
            const FieldStub& field = stub.fields[j];
            String field_key = make_qualified_key(stub.name, field.name);
            SymbolLocation field_location;
            field_location.uri = uri;
            field_location.range = field.range;
            field_location.name_range = field.name_range;
            m_fields[field_key] = field_location;
            name_set.field_keys.push_back(field_key);
        }
    }

    // Index enums
    for (u32 i = 0; i < stubs.enums.size(); i++) {
        const EnumStub& stub = stubs.enums[i];
        String name(stub.name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_enums[name] = location;
        name_set.enum_names.push_back(name);
    }

    // Index functions
    for (u32 i = 0; i < stubs.functions.size(); i++) {
        const FunctionStub& stub = stubs.functions[i];
        String name(stub.name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_functions[name] = location;
        name_set.function_names.push_back(name);
    }

    // Index methods
    for (u32 i = 0; i < stubs.methods.size(); i++) {
        const MethodStub& stub = stubs.methods[i];
        String key = make_qualified_key(stub.struct_name, stub.method_name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_methods[key] = location;
        name_set.method_keys.push_back(key);
    }

    // Index constructors
    for (u32 i = 0; i < stubs.constructors.size(); i++) {
        const ConstructorStub& stub = stubs.constructors[i];
        String key = make_qualified_key(stub.struct_name, stub.constructor_name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_constructors[key] = location;
        name_set.constructor_keys.push_back(key);
    }

    // Index traits
    for (u32 i = 0; i < stubs.traits.size(); i++) {
        const TraitStub& stub = stubs.traits[i];
        String name(stub.name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_traits[name] = location;
        name_set.trait_names.push_back(name);
    }

    // Index globals
    for (u32 i = 0; i < stubs.globals.size(); i++) {
        const GlobalVarStub& stub = stubs.globals[i];
        String name(stub.name);
        SymbolLocation location;
        location.uri = uri;
        location.range = stub.range;
        location.name_range = stub.name_range;
        m_globals[name] = location;
        name_set.global_names.push_back(name);
    }

    m_file_names[uri] = std::move(name_set);
}

void GlobalIndex::remove_file(StringView uri) {
    String uri_key(uri);
    auto file_it = m_file_names.find(uri_key);
    if (file_it == m_file_names.end()) return;

    const FileNameSet& name_set = file_it->second;

    for (u32 i = 0; i < name_set.struct_names.size(); i++) {
        m_structs.erase(name_set.struct_names[i]);
    }
    for (u32 i = 0; i < name_set.enum_names.size(); i++) {
        m_enums.erase(name_set.enum_names[i]);
    }
    for (u32 i = 0; i < name_set.function_names.size(); i++) {
        m_functions.erase(name_set.function_names[i]);
    }
    for (u32 i = 0; i < name_set.trait_names.size(); i++) {
        m_traits.erase(name_set.trait_names[i]);
    }
    for (u32 i = 0; i < name_set.global_names.size(); i++) {
        m_globals.erase(name_set.global_names[i]);
    }
    for (u32 i = 0; i < name_set.method_keys.size(); i++) {
        m_methods.erase(name_set.method_keys[i]);
    }
    for (u32 i = 0; i < name_set.constructor_keys.size(); i++) {
        m_constructors.erase(name_set.constructor_keys[i]);
    }
    for (u32 i = 0; i < name_set.field_keys.size(); i++) {
        m_fields.erase(name_set.field_keys[i]);
    }

    m_file_names.erase(file_it);
}

const SymbolLocation* GlobalIndex::find_struct(StringView name) const {
    auto it = m_structs.find(String(name));
    if (it != m_structs.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_enum(StringView name) const {
    auto it = m_enums.find(String(name));
    if (it != m_enums.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_function(StringView name) const {
    auto it = m_functions.find(String(name));
    if (it != m_functions.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_trait(StringView name) const {
    auto it = m_traits.find(String(name));
    if (it != m_traits.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_global(StringView name) const {
    auto it = m_globals.find(String(name));
    if (it != m_globals.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_method(StringView struct_name, StringView method_name) const {
    String key = make_qualified_key(struct_name, method_name);
    auto it = m_methods.find(key);
    if (it != m_methods.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_constructor(StringView struct_name, StringView constructor_name) const {
    String key = make_qualified_key(struct_name, constructor_name);
    auto it = m_constructors.find(key);
    if (it != m_constructors.end()) return &it->second;
    return nullptr;
}

const SymbolLocation* GlobalIndex::find_field(StringView struct_name, StringView field_name) const {
    String key = make_qualified_key(struct_name, field_name);
    auto it = m_fields.find(key);
    if (it != m_fields.end()) return &it->second;
    return nullptr;
}

Vector<SymbolLocation> GlobalIndex::find_any(StringView name) const {
    Vector<SymbolLocation> results;
    String name_str(name);

    auto struct_it = m_structs.find(name_str);
    if (struct_it != m_structs.end()) results.push_back(struct_it->second);

    auto enum_it = m_enums.find(name_str);
    if (enum_it != m_enums.end()) results.push_back(enum_it->second);

    auto func_it = m_functions.find(name_str);
    if (func_it != m_functions.end()) results.push_back(func_it->second);

    auto trait_it = m_traits.find(name_str);
    if (trait_it != m_traits.end()) results.push_back(trait_it->second);

    auto global_it = m_globals.find(name_str);
    if (global_it != m_globals.end()) results.push_back(global_it->second);

    return results;
}

} // namespace rx

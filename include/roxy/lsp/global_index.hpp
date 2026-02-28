#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/lsp/syntax_tree.hpp"
#include "roxy/lsp/indexer.hpp"

namespace rx {

struct SymbolLocation {
    String uri;           // Owned URI of the file containing the definition
    TextRange range;      // Full declaration range (byte offsets)
    TextRange name_range; // Name token range (byte offsets)
};

class GlobalIndex {
public:
    // Rebuild index entries for a file. Removes old entries first.
    void update_file(const String& uri, const FileStubs& stubs);
    void remove_file(StringView uri);

    // Name lookups — return nullptr if not found
    const SymbolLocation* find_struct(StringView name) const;
    const SymbolLocation* find_enum(StringView name) const;
    const SymbolLocation* find_function(StringView name) const;
    const SymbolLocation* find_trait(StringView name) const;
    const SymbolLocation* find_global(StringView name) const;

    // Qualified lookups — key = "StructName.memberName"
    const SymbolLocation* find_method(StringView struct_name, StringView method_name) const;
    const SymbolLocation* find_constructor(StringView struct_name, StringView constructor_name) const;
    const SymbolLocation* find_field(StringView struct_name, StringView field_name) const;

    // Search all categories for a name, return all matches
    Vector<SymbolLocation> find_any(StringView name) const;

    // Type information queries
    StringView find_struct_parent(StringView struct_name) const;
    StringView find_field_type(StringView struct_name, StringView field_name) const;
    StringView find_function_return_type(StringView function_name) const;
    StringView find_method_return_type(StringView struct_name, StringView method_name) const;

private:
    // Owned string keys -> SymbolLocation
    tsl::robin_map<String, SymbolLocation> m_structs;
    tsl::robin_map<String, SymbolLocation> m_enums;
    tsl::robin_map<String, SymbolLocation> m_functions;
    tsl::robin_map<String, SymbolLocation> m_traits;
    tsl::robin_map<String, SymbolLocation> m_globals;
    tsl::robin_map<String, SymbolLocation> m_methods;       // "Struct.method"
    tsl::robin_map<String, SymbolLocation> m_constructors;  // "Struct.ctor_name"
    tsl::robin_map<String, SymbolLocation> m_fields;        // "Struct.field"

    // Type information maps
    tsl::robin_map<String, String> m_struct_parents;          // "Child" → "Base"
    tsl::robin_map<String, String> m_field_types;             // "Point.x" → "f32"
    tsl::robin_map<String, String> m_function_return_types;   // "get_point" → "Point"
    tsl::robin_map<String, String> m_method_return_types;     // "Point.length" → "f32"

    // Track which names each file contributed (for remove_file)
    struct FileNameSet {
        Vector<String> struct_names;
        Vector<String> enum_names;
        Vector<String> function_names;
        Vector<String> trait_names;
        Vector<String> global_names;
        Vector<String> method_keys;
        Vector<String> constructor_keys;
        Vector<String> field_keys;
        Vector<String> struct_parent_keys;
        Vector<String> field_type_keys;
        Vector<String> function_return_type_keys;
        Vector<String> method_return_type_keys;
    };
    tsl::robin_map<String, FileNameSet> m_file_names;

    // Helper to build qualified key "Struct.member"
    static String make_qualified_key(StringView struct_name, StringView member_name);
};

} // namespace rx

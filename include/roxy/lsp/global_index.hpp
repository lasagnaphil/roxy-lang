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
    };
    tsl::robin_map<String, FileNameSet> m_file_names;

    // Helper to build qualified key "Struct.member"
    static String make_qualified_key(StringView struct_name, StringView member_name);
};

} // namespace rx

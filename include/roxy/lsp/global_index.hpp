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

    // Completion enumeration — lists of names by category
    const Vector<String>* get_struct_fields(StringView struct_name) const;
    const Vector<String>* get_struct_methods(StringView struct_name) const;
    const Vector<String>* get_enum_variants(StringView enum_name) const;

    // Signature queries for completion detail
    StringView find_method_signature(StringView struct_name, StringView method_name) const;
    StringView find_function_signature(StringView function_name) const;

    // Global variable type lookup (for hover)
    StringView find_global_type(StringView name) const;

    // Iterate all names in a category (for bare identifier / type completions)
    template<typename Callback> void for_each_struct(Callback&& cb) const {
        for (auto it = m_structs.begin(); it != m_structs.end(); ++it) {
            cb(it->first);
        }
    }
    template<typename Callback> void for_each_enum(Callback&& cb) const {
        for (auto it = m_enums.begin(); it != m_enums.end(); ++it) {
            cb(it->first);
        }
    }
    template<typename Callback> void for_each_function(Callback&& cb) const {
        for (auto it = m_functions.begin(); it != m_functions.end(); ++it) {
            cb(it->first);
        }
    }
    template<typename Callback> void for_each_trait(Callback&& cb) const {
        for (auto it = m_traits.begin(); it != m_traits.end(); ++it) {
            cb(it->first);
        }
    }
    template<typename Callback> void for_each_global(Callback&& cb) const {
        for (auto it = m_globals.begin(); it != m_globals.end(); ++it) {
            cb(it->first);
        }
    }

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

    // Global variable types
    tsl::robin_map<String, String> m_global_types;           // "count" → "i32"

    // Completion secondary indexes
    tsl::robin_map<String, Vector<String>> m_struct_field_names;   // "Point" → ["x", "y"]
    tsl::robin_map<String, Vector<String>> m_struct_method_names;  // "Point" → ["length", "sum"]
    tsl::robin_map<String, Vector<String>> m_enum_variant_names;   // "Color" → ["Red", "Green", "Blue"]
    tsl::robin_map<String, String> m_function_signatures;          // "add" → "(a: i32, b: i32): i32"
    tsl::robin_map<String, String> m_method_signatures;            // "Point.length" → "(): f32"

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
        Vector<String> global_type_keys;             // global names with types
        Vector<String> struct_field_name_keys;     // struct names with field lists
        Vector<String> struct_method_name_keys;    // struct names with method lists
        Vector<String> enum_variant_name_keys;     // enum names with variant lists
        Vector<String> function_signature_keys;    // function names with signatures
        Vector<String> method_signature_keys;      // "Struct.method" keys with signatures
    };
    tsl::robin_map<String, FileNameSet> m_file_names;

    // Helper to build qualified key "Struct.member"
    static String make_qualified_key(StringView struct_name, StringView member_name);
};

} // namespace rx

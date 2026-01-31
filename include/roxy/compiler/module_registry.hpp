#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/vm/binding/registry.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Forward declarations
struct Decl;

// Kind of export from a module
enum class ExportKind : u8 {
    Function,
    Struct,
    Enum,
};

// Represents a single exported symbol from a module
struct ModuleExport {
    StringView name;        // Symbol name
    ExportKind kind;        // What kind of symbol
    Type* type;             // Type (function type for functions)
    bool is_native;         // True if this is a native function
    bool is_pub;            // True if publicly visible
    u32 index;              // Index in module's function/struct/enum array
    Decl* decl;             // AST declaration (nullptr for native)
};

// Information about a module
struct ModuleInfo {
    StringView name;                    // Module name
    Vector<ModuleExport> exports;       // All exports
    NativeRegistry* natives;            // For native modules (nullptr for script modules)
    bool is_native;                     // True if this is a native-only module

    // Lookup an export by name
    const ModuleExport* find_export(StringView symbol_name) const {
        for (const auto& exp : exports) {
            if (exp.name == symbol_name) {
                return &exp;
            }
        }
        return nullptr;
    }
};

// Helper function to convert NativeTypeKind to Type*
Type* type_from_kind(NativeTypeKind kind, TypeCache& types);

// Registry for all modules in the compilation
class ModuleRegistry {
public:
    explicit ModuleRegistry(BumpAllocator& allocator)
        : m_allocator(allocator)
    {}

    // Register a native module (C++ functions exposed to Roxy)
    // The TypeCache is used to resolve types for the native functions
    void register_native_module(StringView name, NativeRegistry* natives, TypeCache& types);

    // Register a script module (parsed Roxy source)
    // This should be called after semantic analysis of the module
    ModuleInfo* register_script_module(StringView name);

    // Add an export to a script module
    void add_export(ModuleInfo* module, StringView name, ExportKind kind,
                   Type* type, bool is_pub, u32 index, Decl* decl = nullptr);

    // Find a module by name
    ModuleInfo* find_module(StringView name) const {
        auto it = m_modules.find(name);
        if (it != m_modules.end()) {
            return it->second;
        }
        return nullptr;
    }

    // Find an export within a module
    const ModuleExport* find_export(ModuleInfo* module, StringView name) const {
        return module ? module->find_export(name) : nullptr;
    }

    // Get all registered modules
    const tsl::robin_map<StringView, ModuleInfo*, StringViewHash, StringViewEqual>& modules() const {
        return m_modules;
    }

private:
    BumpAllocator& m_allocator;
    tsl::robin_map<StringView, ModuleInfo*, StringViewHash, StringViewEqual> m_modules;
};

}

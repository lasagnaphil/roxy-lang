#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/bytecode.hpp"
#include "roxy/vm/binding/registry.hpp"

namespace rx {

// Forward declarations
struct Program;
struct IRModule;

// Source module - represents a single source file with its module name
struct SourceModule {
    StringView name;        // Module name (e.g., "math", "utils")
    const char* source;     // Source code
    u32 length;             // Source length
};

// Compiler - compiles multiple source modules into a single linked BCModule
//
// Usage:
//   Compiler compiler(allocator);
//   compiler.add_native_registry("builtin", &builtin_natives);
//   compiler.add_source("main", main_source, main_len);
//   compiler.add_source("utils", utils_source, utils_len);
//   BCModule* module = compiler.compile();
//
class Compiler {
public:
    explicit Compiler(BumpAllocator& allocator);

    // Add a native function registry (for built-in modules like "math")
    void add_native_registry(StringView module_name, NativeRegistry* registry);

    // Add a source module to be compiled
    void add_source(StringView module_name, const char* source, u32 length);

    // Compile all modules into a single linked BCModule
    // Returns nullptr on error (check errors() for details)
    BCModule* compile();

    // Error reporting
    bool has_errors() const { return !m_errors.empty(); }
    const Vector<const char*>& errors() const { return m_errors; }

private:
    // Compilation phases
    bool parse_all();
    bool analyze_all();
    bool build_ir_all();
    BCModule* link_modules();

    // Topologically sort modules by import dependencies
    // Returns false and adds error if cycle detected
    bool topological_sort();

    // Detect import cycles using DFS
    bool detect_cycle(u32 module_idx, Vector<u8>& state, Vector<u32>& order);

    // Error reporting
    void add_error(const char* message);
    void add_error_fmt(const char* fmt, ...);

    BumpAllocator& m_allocator;
    TypeCache m_types;
    ModuleRegistry m_module_registry;

    // Builtin module registry (auto-imported as prelude)
    UniquePtr<NativeRegistry> m_builtin_registry;

    // Source modules
    Vector<SourceModule> m_sources;

    // Per-module compilation state (parallel to m_sources)
    struct ModuleState {
        Program* program;           // Parsed AST
        IRModule* ir_module;        // Generated IR
        Vector<StringView> imports; // Module names this module imports
    };
    Vector<ModuleState> m_module_states;

    // Compilation order (after topological sort)
    Vector<u32> m_compile_order;

    // Native registries by module name
    Vector<std::pair<StringView, NativeRegistry*>> m_native_registries;

    // Combined registry with all native functions (built during compile())
    UniquePtr<NativeRegistry> m_combined_registry;

    // Errors
    Vector<const char*> m_errors;
};

}

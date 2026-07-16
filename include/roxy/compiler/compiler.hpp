#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/format.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/compiler/symbol_table.hpp"
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

// Per-compile wall-clock breakdown, in nanoseconds. Always populated by
// compile() (the steady_clock overhead is a handful of calls per compile, so
// there is no reason to gate it). `total_ns` is the whole compile(); the named
// phases sum to slightly less than it — the remainder (merge, native binding,
// registry setup) is the pipeline's "other" bucket. See `roxy --time`.
struct CompileTimings {
    u64 parse_ns = 0;        // Phase 1: lexer + parser, all modules
    u64 topo_ns = 0;         // Phase 2: topological sort by imports
    u64 sema_ns = 0;         // Phase 3: semantic analysis, all modules
    u64 ir_build_ns = 0;     // Phase 4: AST -> SSA IR, all modules
    u64 coro_lower_ns = 0;   // Phase 5a: coroutine state-machine lowering
    u64 ir_optimize_ns = 0;  // Phase 5b: SSA IR optimization passes
    u64 ir_validate_ns = 0;  // Phase 5c: IR structural validation
    u64 bc_lower_ns = 0;     // Phase 5d: SSA IR -> bytecode (incl. regalloc)
    u64 total_ns = 0;        // Whole compile() call
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

    // Access IR modules (valid after compile() succeeds)
    u32 module_count() const { return static_cast<u32>(m_module_states.size()); }
    IRModule* ir_module(u32 index) const { return m_module_states[index].ir_module; }

    // Per-phase wall-clock breakdown of the last compile() call.
    const CompileTimings& timings() const { return m_timings; }

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
    template<typename... Args>
    void add_error_fmt(fmt_string<sizeof...(Args)> fmt, const Args&... args) {
        char buffer[512];
        format_to(buffer, sizeof(buffer), runtime_format_string{fmt.str}, args...);
        add_error(buffer);
    }

    BumpAllocator& m_allocator;
    TypeEnv m_type_env;
    ModuleRegistry m_module_registry;

    // Builtin module registry (auto-imported as prelude)
    UniquePtr<NativeRegistry> m_builtin_registry;

    // Source modules
    Vector<SourceModule> m_sources;

    // Per-module compilation state (parallel to m_sources)
    struct ModuleState {
        Program* program = nullptr;           // Parsed AST
        IRModule* ir_module = nullptr;        // Generated IR
        Vector<StringView> imports;           // Module names this module imports
        SymbolTable* symbols = nullptr;       // Persisted from analysis (owned)
        Vector<Decl*> synthetic_decls;        // Persisted from analysis
    };
    Vector<ModuleState> m_module_states;

    // Compilation order (after topological sort)
    Vector<u32> m_compile_order;

    // Native registries by module name
    Vector<std::pair<StringView, NativeRegistry*>> m_native_registries;

    // Combined registry with all native functions (built during compile())
    UniquePtr<NativeRegistry> m_combined_registry;

    // Per-phase wall-clock breakdown of the last compile() (see timings()).
    CompileTimings m_timings;

    // Errors
    Vector<const char*> m_errors;
};

}

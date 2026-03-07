#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/tsl/robin_map.h"
#include "roxy/core/tsl/robin_set.h"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/compiler/symbol_table.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/lsp/syntax_tree.hpp"
#include "roxy/lsp/indexer.hpp"

namespace rx {

// Forward declarations
class NativeRegistry;
class CstLowering;

// Result of analyzing a single function body
struct BodyAnalysisResult {
    Decl* decl = nullptr;               // The lowered AST declaration
    SymbolTable* symbols = nullptr;      // Symbol table populated during analysis
    Vector<SemanticError> errors;         // Any semantic errors found
    bool success = false;                // true if analysis completed without fatal errors
};

// LspAnalysisContext owns persistent type-system state and provides
// full semantic analysis for the LSP server. It mirrors the compiler's
// multi-pass analysis (passes 0-2 for declarations, pass 3 for bodies)
// but is designed for interactive, error-tolerant use.
class LspAnalysisContext {
public:
    LspAnalysisContext();
    ~LspAnalysisContext();

    // Rebuild all declaration-level types from workspace source files.
    // Takes a list of (uri, source, source_length) tuples for all workspace files.
    // Resets type state and re-runs passes 0-2 on all files.
    struct SourceFile {
        StringView uri;
        const char* source;
        u32 source_length;
        SyntaxNode* cst_root;  // Pre-parsed CST root
    };
    void rebuild_declarations(Span<SourceFile> files);

    // Analyze a single function body on demand (pass 3 equivalent).
    // The CST node should be a function/method/constructor/destructor.
    // Returns analysis result with resolved types and symbols.
    BodyAnalysisResult analyze_function_body(SyntaxNode* fn_cst,
                                             BumpAllocator& ast_allocator);

    // Access to type system for formatting/lookups
    TypeEnv& type_env() { return *m_type_env; }
    TypeCache& types() { return m_type_env->types(); }

    // Declaration version (incremented on each rebuild)
    u64 declaration_version() const { return m_declaration_version; }

    // Whether declarations have been built at least once
    bool is_initialized() const { return m_initialized; }

    // Format a Type* as a human-readable string
    static String type_to_string(Type* type);

    // Collect all local variable/parameter names and their resolved types from
    // an analyzed function AST. Walks the function body to build a flat map.
    // Includes "self" for method/constructor/destructor decls.
    void collect_local_variables(Decl* analyzed_decl,
                                 tsl::robin_map<String, Type*>& out_vars);

    // Collect just local variable/parameter names from an analyzed function AST
    // (no type info needed). Used for fast local-variable disambiguation in
    // find-references and rename.
    static void collect_local_var_names(Decl* analyzed_decl,
                                        tsl::robin_set<String>& out_names);

    // Resolve the type of a CST expression using collected local variables
    // and the type system. Returns nullptr if unresolvable.
    Type* resolve_cst_expr_type(SyntaxNode* expr_node,
                                const tsl::robin_map<String, Type*>& local_vars);

private:
    // Single allocator for types and declaration ASTs — reset on each
    // rebuild_declarations(). Mirrors the compiler's single-allocator pattern.
    UniquePtr<BumpAllocator> m_type_allocator;

    // Persistent type system state
    UniquePtr<TypeEnv> m_type_env;
    UniquePtr<ModuleRegistry> m_module_registry;
    UniquePtr<NativeRegistry> m_builtin_registry;

    // Declaration-level symbol table (persists between body analyses)
    UniquePtr<SymbolTable> m_decl_symbols;

    // Program from last declaration rebuild (persists for body analysis lookups)
    Program* m_decl_program = nullptr;

    u64 m_declaration_version = 0;
    bool m_initialized = false;

    // Initialize the builtin module and registry
    void init_builtins();
};

} // namespace rx

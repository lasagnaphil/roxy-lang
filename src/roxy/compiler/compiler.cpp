#include "roxy/compiler/compiler.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/ir_validator.hpp"
#include "roxy/compiler/coroutine_lowering.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/vm/binding/registry.hpp"
#include "roxy/vm/natives.hpp"

#include <cstring>

namespace rx {

Compiler::Compiler(BumpAllocator& allocator)
    : m_allocator(allocator)
    , m_type_env(allocator)
    , m_module_registry(allocator)
    , m_builtin_registry(new NativeRegistry(allocator, m_type_env.types()))
{
    // Register built-in natives and add as "builtin" module (auto-imported as prelude)
    register_builtin_natives(*m_builtin_registry);
    m_module_registry.register_native_module(BUILTIN_MODULE_NAME, m_builtin_registry.get(), m_type_env.types());
    m_native_registries.push_back({BUILTIN_MODULE_NAME, m_builtin_registry.get()});
}

void Compiler::add_native_registry(StringView module_name, NativeRegistry* registry) {
    m_native_registries.push_back({module_name, registry});
    m_module_registry.register_native_module(module_name, registry, m_type_env.types());
}

void Compiler::add_source(StringView module_name, const char* source, u32 length) {
    SourceModule src;
    src.name = module_name;
    src.source = source;
    src.length = length;
    m_sources.push_back(src);

    // Register as a script module (exports will be populated during analysis)
    m_module_registry.register_script_module(module_name);
}

BCModule* Compiler::compile() {
    // Build combined registry with all native functions from all registries
    m_combined_registry = make_unique<NativeRegistry>(m_allocator, m_type_env.types());
    for (const auto& [name, registry] : m_native_registries) {
        registry->copy_entries_to(*m_combined_registry);
    }

    // Initialize per-module state
    m_module_states.resize(m_sources.size());

    // Phase 1: Parse all modules
    if (!parse_all()) {
        return nullptr;
    }

    // Phase 2: Topologically sort by imports
    if (!topological_sort()) {
        return nullptr;
    }

    // Phase 3: Semantic analysis (in topological order)
    if (!analyze_all()) {
        return nullptr;
    }

    // Phase 4: Build IR for all modules
    if (!build_ir_all()) {
        return nullptr;
    }

    // Phase 5: Link into single BCModule
    return link_modules();
}

bool Compiler::parse_all() {
    for (u32 i = 0; i < m_sources.size(); i++) {
        const SourceModule& src = m_sources[i];

        Lexer lexer(src.source, src.length);
        Parser parser(lexer, m_allocator);
        Program* program = parser.parse();

        if (!program || parser.has_error()) {
            const auto& err = parser.error();
            add_error_fmt("Parse error in module '{}' at line {}: {}",
                         src.name, err.loc.line, err.message);
            return false;
        }

        m_module_states[i].program = program;

        // Collect imports from the program
        for (auto* decl : program->declarations) {
            if (decl && decl->kind == AstKind::DeclImport) {
                m_module_states[i].imports.push_back(decl->import_decl.module_path);
            }
        }
    }

    return true;
}

bool Compiler::topological_sort() {
    // Build module name to index map
    tsl::robin_map<StringView, u32> name_to_idx;
    for (u32 i = 0; i < m_sources.size(); i++) {
        name_to_idx[m_sources[i].name] = i;
    }

    // State: 0 = unvisited, 1 = visiting, 2 = visited
    Vector<u8> state(m_sources.size(), 0);
    m_compile_order.clear();

    for (u32 i = 0; i < m_sources.size(); i++) {
        if (state[i] == 0) {
            if (!detect_cycle(i, state, m_compile_order)) {
                return false;
            }
        }
    }

    // m_compile_order is now in reverse topological order (dependencies first)
    return true;
}

bool Compiler::detect_cycle(u32 module_idx, Vector<u8>& state, Vector<u32>& order) {
    state[module_idx] = 1; // Visiting

    // Build module name to index map (could cache this)
    tsl::robin_map<StringView, u32> name_to_idx;
    for (u32 i = 0; i < m_sources.size(); i++) {
        name_to_idx[m_sources[i].name] = i;
    }

    // Check all imports
    for (const StringView& import_name : m_module_states[module_idx].imports) {
        auto it = name_to_idx.find(import_name);
        if (it == name_to_idx.end()) {
            // Import is not a script module (could be native module)
            continue;
        }

        u32 dep_idx = it->second;
        if (state[dep_idx] == 1) {
            // Cycle detected
            add_error_fmt("Circular import detected: module '{}' imports '{}' which creates a cycle",
                         m_sources[module_idx].name, import_name);
            return false;
        }

        if (state[dep_idx] == 0) {
            if (!detect_cycle(dep_idx, state, order)) {
                return false;
            }
        }
    }

    state[module_idx] = 2; // Visited
    order.push_back(module_idx);
    return true;
}

bool Compiler::analyze_all() {
    // Analyze in topological order (dependencies first)
    for (u32 idx : m_compile_order) {
        const SourceModule& src = m_sources[idx];
        Program* program = m_module_states[idx].program;

        // Set module name on program for visibility checking
        program->module_name = src.name;

        // Allocate an external SymbolTable that persists after analyzer scope
        auto* symbols = new SymbolTable(m_allocator);

        // Use the shared TypeEnv for type consistency, with external symbol table
        SemanticAnalyzer analyzer(m_allocator, m_type_env, m_module_registry, *symbols);

        if (!analyzer.analyze(program)) {
            for (const auto& err : analyzer.errors()) {
                add_error_fmt("Semantic error in module '{}' at line {}: {}",
                             src.name, err.loc.line, err.message);
            }
            delete symbols;
            return false;
        }

        // Persist symbols and synthetic decls for build_ir_all()
        m_module_states[idx].symbols = symbols;
        const auto& syn_vec = analyzer.synthetic_decls();
        for (auto* d : syn_vec) {
            m_module_states[idx].synthetic_decls.push_back(d);
        }

        // After analysis, register this module's exports
        ModuleInfo* mod_info = m_module_registry.find_module(src.name);
        if (mod_info) {
            // Register public functions as exports
            // Look up function types from the symbol table
            for (auto* decl : program->declarations) {
                if (decl && decl->kind == AstKind::DeclFun && decl->fun_decl.is_pub) {
                    // Look up the function in the symbol table to get its type
                    Symbol* sym = symbols->lookup(decl->fun_decl.name);
                    Type* func_type = sym ? sym->type : nullptr;

                    ModuleExport exp;
                    exp.name = decl->fun_decl.name;
                    exp.kind = ExportKind::Function;
                    exp.type = func_type;
                    exp.is_native = false;
                    exp.is_pub = true;
                    exp.index = static_cast<u32>(mod_info->exports.size());
                    exp.decl = decl;
                    mod_info->exports.push_back(exp);
                }
            }
        }
    }

    return true;
}

bool Compiler::build_ir_all() {
    // Build IR in topological order — no re-analysis needed,
    // symbols and synthetic_decls were persisted during analyze_all()
    for (u32 idx : m_compile_order) {
        const SourceModule& src = m_sources[idx];
        Program* program = m_module_states[idx].program;
        SymbolTable& symbols = *m_module_states[idx].symbols;

        // Build synthetic_decls span from persisted vector
        auto& syn_vec = m_module_states[idx].synthetic_decls;
        Span<Decl*> synthetic_decls;
        if (!syn_vec.empty()) {
            Decl** data = reinterpret_cast<Decl**>(m_allocator.alloc_bytes(
                sizeof(Decl*) * syn_vec.size(), alignof(Decl*)));
            for (u32 j = 0; j < syn_vec.size(); j++) {
                data[j] = syn_vec[j];
            }
            synthetic_decls = Span<Decl*>(data, static_cast<u32>(syn_vec.size()));
        }

        // Use combined registry with all native functions
        IRBuilder ir_builder(m_allocator, m_type_env, *m_combined_registry, symbols, m_module_registry);
        IRModule* ir_module = ir_builder.build(program, synthetic_decls);

        if (!ir_module) {
            if (ir_builder.has_error()) {
                add_error_fmt("IR generation failed for module '{}': {}",
                             src.name, ir_builder.error());
            } else {
                add_error_fmt("IR generation failed for module '{}'",
                             src.name);
            }
            return false;
        }

        m_module_states[idx].ir_module = ir_module;
    }

    return true;
}

BCModule* Compiler::link_modules() {
    // For now, merge all IR modules and build a single bytecode module
    // This is a simplified linker that combines functions from all modules

    // Create merged IR module
    IRModule merged_ir;
    merged_ir.name = "linked";

    // Map from (module_name, func_name) to global function index
    tsl::robin_map<StringView, u32> func_name_to_global_idx;

    // Collect all functions
    for (u32 idx : m_compile_order) {
        IRModule* ir_mod = m_module_states[idx].ir_module;
        StringView mod_name = m_sources[idx].name;

        for (IRFunction* func : ir_mod->functions) {
            // For linked module, we may need to prefix function names to avoid conflicts
            // For now, assume function names are unique across modules
            u32 global_idx = static_cast<u32>(merged_ir.functions.size());
            func_name_to_global_idx[func->name] = global_idx;
            merged_ir.functions.push_back(func);
        }
    }

    // Coroutine lowering pass: transform coroutine functions into init/resume/done
    coroutine_lower(&merged_ir, m_allocator, m_type_env);

    // Validate merged IR before lowering
    IRValidator validator;
    if (!validator.validate(&merged_ir)) {
        add_error_fmt("IR validation failed: {}", validator.error());
        return nullptr;
    }

    // Build bytecode from merged IR
    // Static linking: all cross-module calls are resolved in the lowering phase
    // since m_func_indices contains all functions from all modules
    BytecodeBuilder bc_builder;
    bc_builder.set_registry(m_combined_registry.get());
    bc_builder.set_type_env(&m_type_env);
    BCModule* module = bc_builder.build(&merged_ir);

    if (!module) {
        add_error("Bytecode generation failed during linking");
        return nullptr;
    }

    // Apply native functions from combined registry
    m_combined_registry->apply_to_module(module);

    return module;
}

void Compiler::add_error(const char* message) {
    // Copy message to allocator
    u32 len = static_cast<u32>(strlen(message));
    char* msg = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
    memcpy(msg, message, len + 1);
    m_errors.push_back(msg);
}


} // namespace rx

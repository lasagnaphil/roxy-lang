// Roxy standalone interpreter
// Usage: roxy <source_file>

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/file.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/interop.hpp"

#include <cstdio>
#include <cstring>

using namespace rx;

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options] <source_file>\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  source_file    Path to a .roxy source file\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --ir           Print generated SSA IR\n");
    fprintf(stderr, "  --help, -h     Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "The program must define a main() function as the entry point.\n");
}

struct Options {
    const char* source_file = nullptr;
    bool print_ir = false;
};

static bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (strcmp(argv[i], "--ir") == 0) {
            opts.print_ir = true;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        } else if (!opts.source_file) {
            opts.source_file = argv[i];
        } else {
            fprintf(stderr, "Error: Unexpected argument '%s'\n", argv[i]);
            return false;
        }
    }

    if (!opts.source_file) {
        fprintf(stderr, "Error: No source file specified\n\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

static BCModule* compile(BumpAllocator& allocator, const char* source, u32 len,
                         TypeEnv& type_env, NativeRegistry& registry, bool print_ir) {
    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        const auto& err = parser.error();
        fprintf(stderr, "Parse error at line %u, column %u: %s\n",
                err.loc.line, err.loc.column, err.message);
        return nullptr;
    }

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        fprintf(stderr, "Semantic errors:\n");
        for (const auto& err : analyzer.errors()) {
            fprintf(stderr, "  Line %u, column %u: %s\n",
                    err.loc.line, err.loc.column, err.message);
        }
        return nullptr;
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
    IRModule* ir_module = ir_builder.build(program);
    if (!ir_module) {
        fprintf(stderr, "IR generation failed\n");
        return nullptr;
    }

    if (print_ir) {
        Vector<char> ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        printf("=== SSA IR ===\n%s\n", ir_str.data());
    }

    BytecodeBuilder bc_builder;
    bc_builder.set_registry(&registry);
    BCModule* module = bc_builder.build(ir_module);
    if (!module) {
        fprintf(stderr, "Bytecode generation failed\n");
        return nullptr;
    }

    registry.apply_to_module(module);
    return module;
}

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return 1;
    }

    // Read source file
    Vector<u8> source_buf;
    if (!read_file_to_buf(opts.source_file, source_buf)) {
        fprintf(stderr, "Error: Could not read file '%s'\n", opts.source_file);
        return 1;
    }

    // read_file_to_buf already null-terminates, so size includes the null
    const char* source = reinterpret_cast<const char*>(source_buf.data());
    u32 len = static_cast<u32>(source_buf.size() - 1);  // Exclude null terminator

    // Create allocator and type environment
    BumpAllocator allocator(65536);
    TypeEnv type_env(allocator);

    // Create registry and register built-in natives
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    // Compile
    UniquePtr<BCModule> module(compile(allocator, source, len, type_env, registry, opts.print_ir));
    if (!module) {
        return 1;
    }

    // Find main() function
    StringView main_name("main", 4);
    BCFunction* main_func = nullptr;
    for (auto& fn : module->functions) {
        if (fn->name == main_name) {
            main_func = fn.get();
            break;
        }
    }

    if (!main_func) {
        fprintf(stderr, "Error: No main() function found\n");
        return 1;
    }

    if (main_func->param_count > 0) {
        fprintf(stderr, "Error: main() must take no arguments (found %u parameters)\n",
                main_func->param_count);
        return 1;
    }

    // Initialize VM and run
    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module.get());

    if (!vm_call(&vm, main_name, {})) {
        fprintf(stderr, "Runtime error: %s\n", vm.error ? vm.error : "unknown error");
        vm_destroy(&vm);
        return 1;
    }

    Value result = vm_get_result(&vm);

    vm_destroy(&vm);

    // Use integer return value as exit code
    if (result.is_int()) {
        return static_cast<int>(result.as_int);
    }
    return 0;
}

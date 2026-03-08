// Roxy standalone interpreter
// Usage: roxy [options] <source_file> [program_args...]

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/file.hpp"
#include "roxy/core/unique_ptr.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/compiler.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/string.hpp"
#include "roxy/vm/list.hpp"

#include <cstdio>
#include <cstring>

// tsl::robin_map for visited module tracking (used as set with bool values)
#include "roxy/core/tsl/robin_map.h"

using namespace rx;

static void print_usage(const char* program) {
    fprintf(stderr, "Usage: %s [options] <source_file> [args...]\n", program);
    fprintf(stderr, "\n");
    fprintf(stderr, "Arguments:\n");
    fprintf(stderr, "  source_file    Path to a .roxy source file\n");
    fprintf(stderr, "  args           Arguments passed to main(args: List<string>)\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "Options:\n");
    fprintf(stderr, "  --help, -h     Show this help message\n");
    fprintf(stderr, "\n");
    fprintf(stderr, "The program must define a main() function as the entry point.\n");
    fprintf(stderr, "Imported modules are auto-discovered from the source file's directory.\n");
}

struct Options {
    const char* source_file = nullptr;
    int program_args_start = 0;  // Index into argv where program args begin (0 = none)
};

static bool parse_args(int argc, char** argv, Options& opts) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            print_usage(argv[0]);
            return false;
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "Unknown option: %s\n", argv[i]);
            return false;
        } else if (!opts.source_file) {
            opts.source_file = argv[i];
            // All remaining arguments are program arguments
            opts.program_args_start = i + 1;
            break;
        }
    }

    if (!opts.source_file) {
        fprintf(stderr, "Error: No source file specified\n\n");
        print_usage(argv[0]);
        return false;
    }

    return true;
}

// Extract the directory part of a file path (including trailing separator).
// Returns "." if no directory separator found.
static String get_directory(const char* path) {
    const char* last_sep = nullptr;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    if (!last_sep) {
        return String("./", 2);
    }
    return String(path, static_cast<u32>(last_sep - path + 1));
}

// Extract the module name from a file path (basename without .roxy extension).
static String get_module_name(const char* path) {
    const char* last_sep = nullptr;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') last_sep = p;
    }
    const char* basename = last_sep ? last_sep + 1 : path;
    u32 basename_len = static_cast<u32>(strlen(basename));

    // Strip .roxy extension if present
    if (basename_len > 5 && strcmp(basename + basename_len - 5, ".roxy") == 0) {
        return String(basename, basename_len - 5);
    }
    return String(basename, basename_len);
}

// Scan source code for import/from statements and extract module names.
// Uses the lexer to correctly handle comments and strings.
static void scan_imports(const char* source, u32 len, Vector<String>& module_names) {
    Lexer lexer(source, len);
    while (true) {
        Token token = lexer.next_token();
        if (token.kind == TokenKind::Eof) break;

        if (token.kind == TokenKind::KwImport || token.kind == TokenKind::KwFrom) {
            // Next token should be the first segment of the module path
            Token name = lexer.next_token();
            if (name.kind == TokenKind::Identifier) {
                // Build full dotted path (e.g., a.b.c)
                String mod_path(name.start, name.length);
                while (true) {
                    Token next = lexer.next_token();
                    if (next.kind == TokenKind::Dot) {
                        Token segment = lexer.next_token();
                        if (segment.kind == TokenKind::Identifier) {
                            mod_path.push_back('.');
                            mod_path.append(segment.start, segment.length);
                        } else {
                            break;
                        }
                    } else {
                        break;
                    }
                }
                module_names.push_back(std::move(mod_path));
            }
        }
    }
}

// Stored source file data - keeps the buffer alive for the compiler
struct SourceFile {
    String module_name;
    Vector<u8> buffer;  // Source bytes (null-terminated by read_file_to_buf)
};

// Recursively discover all imported modules starting from the main file.
// Returns false on error (e.g., missing imported file).
static bool discover_modules(const String& base_dir,
                             const String& module_name,
                             const char* source, u32 source_len,
                             Vector<SourceFile>& discovered,
                             tsl::robin_map<String, bool>& visited) {
    if (visited.count(module_name)) return true;
    visited[module_name] = true;

    // Scan this source for imports
    Vector<String> imports;
    scan_imports(source, source_len, imports);

    // Process each import
    for (auto& import_name : imports) {
        if (visited.count(import_name)) continue;

        // Build file path: base_dir + import_name + ".roxy"
        // For dotted paths like "foo.bar", use "foo.bar.roxy" (flat directory)
        String file_path = base_dir;
        file_path.append(import_name.data(), import_name.size());
        file_path.append(".roxy", 5);
        file_path.push_back('\0');

        // Read the file
        SourceFile source_file;
        source_file.module_name = import_name;
        if (!read_file_to_buf(file_path.data(), source_file.buffer)) {
            fprintf(stderr, "Error: Could not read imported module '%s' (expected at '%s')\n",
                    import_name.c_str(), file_path.data());
            return false;
        }

        const char* mod_source = reinterpret_cast<const char*>(source_file.buffer.data());
        u32 mod_len = static_cast<u32>(source_file.buffer.size() - 1);

        // Recursively discover this module's imports
        if (!discover_modules(base_dir, import_name, mod_source, mod_len,
                              discovered, visited)) {
            return false;
        }

        discovered.push_back(std::move(source_file));
    }

    return true;
}

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        return 1;
    }

    // Read main source file
    Vector<u8> main_source_buf;
    if (!read_file_to_buf(opts.source_file, main_source_buf)) {
        fprintf(stderr, "Error: Could not read file '%s'\n", opts.source_file);
        return 1;
    }

    const char* main_source = reinterpret_cast<const char*>(main_source_buf.data());
    u32 main_len = static_cast<u32>(main_source_buf.size() - 1);

    // Determine base directory and module name
    String base_dir = get_directory(opts.source_file);
    String main_module_name = get_module_name(opts.source_file);

    // Discover all imported modules recursively
    Vector<SourceFile> discovered_modules;
    tsl::robin_map<String, bool> visited;
    if (!discover_modules(base_dir, main_module_name, main_source, main_len,
                          discovered_modules, visited)) {
        return 1;
    }

    // Create allocator and compiler
    BumpAllocator allocator(65536);
    Compiler compiler(allocator);

    // Add discovered modules first (dependencies before dependents)
    for (auto& source_file : discovered_modules) {
        const char* source = reinterpret_cast<const char*>(source_file.buffer.data());
        u32 len = static_cast<u32>(source_file.buffer.size() - 1);
        compiler.add_source(StringView(source_file.module_name.data(),
                                       static_cast<u32>(source_file.module_name.size())),
                           source, len);
    }

    // Add main module last
    compiler.add_source(StringView(main_module_name.data(),
                                   static_cast<u32>(main_module_name.size())),
                       main_source, main_len);

    // Compile all modules
    BCModule* module = compiler.compile();
    if (!module) {
        fprintf(stderr, "Compilation failed:\n");
        for (const char* error : compiler.errors()) {
            fprintf(stderr, "  %s\n", error);
        }
        return 1;
    }

    // Find main() function
    StringView main_func_name("main", 4);
    BCFunction* main_func = nullptr;
    for (auto& fn : module->functions) {
        if (fn->name == main_func_name) {
            main_func = fn.get();
            break;
        }
    }

    if (!main_func) {
        fprintf(stderr, "Error: No main() function found\n");
        return 1;
    }

    if (main_func->param_count > 1) {
        fprintf(stderr, "Error: main() must take 0 or 1 argument (found %u parameters)\n",
                main_func->param_count);
        return 1;
    }

    // Initialize VM and run
    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // Build argument list for main() if it takes a parameter
    Value args_value;
    if (main_func->param_count == 1) {
        // Count program args: source file + remaining CLI arguments
        int program_arg_count = 1;  // source file is args[0]
        if (opts.program_args_start > 0) {
            program_arg_count += argc - opts.program_args_start;
        }

        // Allocate List<string>
        void* list_data = list_alloc(&vm, static_cast<u32>(program_arg_count));

        // args[0] = source file path
        u32 source_path_len = static_cast<u32>(strlen(opts.source_file));
        void* source_str = string_alloc(&vm, opts.source_file, source_path_len);
        list_push(list_data, Value::make_ptr(source_str));

        // args[1..] = remaining CLI arguments
        if (opts.program_args_start > 0) {
            for (int i = opts.program_args_start; i < argc; i++) {
                u32 arg_len = static_cast<u32>(strlen(argv[i]));
                void* arg_str = string_alloc(&vm, argv[i], arg_len);
                list_push(list_data, Value::make_ptr(arg_str));
            }
        }

        args_value = Value::make_ptr(list_data);
    }

    Span<Value> call_args = main_func->param_count == 1
        ? Span<Value>(&args_value, 1)
        : Span<Value>();

    if (!vm_call(&vm, main_func_name, call_args)) {
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

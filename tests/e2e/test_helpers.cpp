#include "test_helpers.hpp"

#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/ir_validator.hpp"
#include "roxy/compiler/coroutine_lowering.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/compiler/c_emitter.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstring>
#include <cstdlib>

#ifdef _WIN32
#define _CRT_SECURE_NO_WARNINGS
#include <io.h>
#include <fcntl.h>
#include <windows.h>
#define dup _dup
#define dup2 _dup2
#define fileno _fileno
#define close _close
#else
#include <unistd.h>
#endif

// Suppress MSVC deprecation warnings for freopen
#ifdef _MSC_VER
#pragma warning(disable: 4996)
#endif

namespace rx {

BCModule* compile(BumpAllocator& allocator, const char* source, bool debug) {
    u32 len = 0;
    while (source[len]) len++;

    // Create type environment and registry
    TypeEnv type_env(allocator);
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        if (debug) {
            printf("Semantic errors:\n");
            for (const auto& err : analyzer.errors()) {
                printf("  Line %u: %s\n", err.loc.line, err.message);
            }
        }
        return nullptr;
    }

    // Capture synthetic declarations (e.g., injected default trait methods)
    const auto& syn_vec = analyzer.synthetic_decls();
    Span<Decl*> synthetic_decls;
    if (!syn_vec.empty()) {
        Decl** data = reinterpret_cast<Decl**>(allocator.alloc_bytes(
            sizeof(Decl*) * syn_vec.size(), alignof(Decl*)));
        for (u32 j = 0; j < syn_vec.size(); j++) {
            data[j] = syn_vec[j];
        }
        synthetic_decls = Span<Decl*>(data, static_cast<u32>(syn_vec.size()));
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
    IRModule* ir_module = ir_builder.build(program, synthetic_decls);
    if (!ir_module) {
        return nullptr;
    }

    if (debug) {
        String ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        printf("=== IR (before coroutine lowering) ===\n%s\n", ir_str.data());
    }

    // Coroutine lowering pass: transform coroutine functions into init/resume/done
    coroutine_lower(ir_module, allocator, type_env);

    if (debug) {
        String ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        printf("=== IR (after coroutine lowering) ===\n%s\n", ir_str.data());
    }

    IRValidator validator;
    if (!validator.validate(ir_module)) {
        if (debug) printf("IR validation failed: %s\n", validator.error());
        return nullptr;
    }

    BytecodeBuilder bc_builder;
    bc_builder.set_registry(&registry);
    bc_builder.set_type_env(&type_env);
    BCModule* module = bc_builder.build(ir_module);
    if (module) {
        // Register native functions with the module for runtime
        registry.apply_to_module(module);
    }
    return module;
}

IRModule* compile_to_ir(BumpAllocator& allocator, const char* source, bool debug) {
    u32 len = 0;
    while (source[len]) len++;

    TypeEnv type_env(allocator);
    NativeRegistry registry(allocator, type_env.types());
    register_builtin_natives(registry);

    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator, type_env, modules);
    if (!analyzer.analyze(program)) {
        if (debug) {
            fprintf(stderr, "Semantic errors:\n");
            for (const auto& err : analyzer.errors()) {
                fprintf(stderr, "  Line %u: %s\n", err.loc.line, err.message);
            }
        }
        return nullptr;
    }

    const auto& syn_vec = analyzer.synthetic_decls();
    Span<Decl*> synthetic_decls;
    if (!syn_vec.empty()) {
        Decl** data = reinterpret_cast<Decl**>(allocator.alloc_bytes(
            sizeof(Decl*) * syn_vec.size(), alignof(Decl*)));
        for (u32 j = 0; j < syn_vec.size(); j++) {
            data[j] = syn_vec[j];
        }
        synthetic_decls = Span<Decl*>(data, static_cast<u32>(syn_vec.size()));
    }

    IRBuilder ir_builder(allocator, type_env, registry, analyzer.symbols(), modules);
    IRModule* ir_module = ir_builder.build(program, synthetic_decls);
    if (!ir_module) {
        return nullptr;
    }

    if (debug) {
        String ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        fprintf(stderr, "=== IR ===\n%s\n", ir_str.data());
    }

    coroutine_lower(ir_module, allocator, type_env);

    IRValidator validator;
    if (!validator.validate(ir_module)) {
        if (debug) fprintf(stderr, "IR validation failed: %s\n", validator.error());
        return nullptr;
    }

    return ir_module;
}

String compile_to_cpp(const char* source, bool debug) {
    BumpAllocator allocator(8192);
    IRModule* ir_module = compile_to_ir(allocator, source, debug);
    if (!ir_module) {
        return String();
    }

    CEmitterConfig config;
    config.emit_main_entry = true;
    CEmitter emitter(allocator, config);

    String output;
    emitter.emit_source(ir_module, output);

    if (debug) {
        printf("=== Generated C++ ===\n%s\n", output.c_str());
    }

    return output;
}

// Get the project root directory
// Uses ROXY_PROJECT_ROOT define set by CMake, falls back to __FILE__-based detection
static const char* get_project_root() {
#ifdef ROXY_PROJECT_ROOT
    return ROXY_PROJECT_ROOT;
#else
    // Fallback: derive from __FILE__
    // __FILE__ = ".../roxy-v2/tests/e2e/test_helpers.cpp"
    static char root[512] = {0};
    if (root[0] != '\0') return root;

    const char* file = __FILE__;
    size_t file_len = strlen(file);
    if (file_len >= sizeof(root)) return nullptr;
    strncpy(root, file, sizeof(root) - 1);

    for (size_t i = file_len; i > 0; i--) {
        if (root[i - 1] == '/' || root[i - 1] == '\\') {
            root[i - 1] = '\0';
            size_t remaining = strlen(root);
            if (remaining >= 9 && strcmp(root + remaining - 9, "tests/e2e") == 0) {
                root[remaining - 9] = '\0';
                remaining = strlen(root);
                if (remaining > 0 && (root[remaining - 1] == '/' || root[remaining - 1] == '\\')) {
                    root[remaining - 1] = '\0';
                }
                return root;
            }
            root[i - 1] = file[i - 1];
        }
    }
    return nullptr;
#endif
}

CBackendResult compile_and_run_cpp(const char* source, bool debug) {
    CBackendResult result;
    result.exit_code = -1;
    result.compile_success = false;
    result.run_success = false;

    String cpp_source = compile_to_cpp(source, debug);
    if (cpp_source.empty()) {
        fprintf(stderr, "[C Backend] compile_to_cpp returned empty\n");
        return result;
    }

    // Find project root for runtime include path
    const char* project_root = get_project_root();
    if (!project_root) {
        fprintf(stderr, "[C Backend] Failed to find project root\n");
        return result;
    }

    // Build paths to runtime files
    char rt_include_dir[512];       // For generated code: #include "roxy_rt.h"
    char rt_include_dir_root[512];  // For roxy_rt.cpp: #include "roxy/rt/roxy_rt.h"
    char rt_src_path[512];
    snprintf(rt_include_dir, sizeof(rt_include_dir), "%s/include/roxy/rt", project_root);
    snprintf(rt_include_dir_root, sizeof(rt_include_dir_root), "%s/include", project_root);
    snprintf(rt_src_path, sizeof(rt_src_path), "%s/src/roxy/rt/roxy_rt.cpp", project_root);

    // Write C++ source to temp file
    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    char src_path[256];
    char bin_path[256];
    snprintf(src_path, sizeof(src_path), "%s/roxy_cbackend_XXXXXX.cpp", tmpdir);
    snprintf(bin_path, sizeof(bin_path), "%s/roxy_cbackend_bin_XXXXXX", tmpdir);

    // Create unique temp file for source
    int src_fd = mkstemps(src_path, 4); // .cpp suffix
    if (src_fd < 0) {
        return result;
    }

    // Write source
    write(src_fd, cpp_source.data(), cpp_source.size());
    close(src_fd);

    // Create unique temp path for binary
    int bin_fd = mkstemp(bin_path);
    if (bin_fd < 0) {
        remove(src_path);
        return result;
    }
    close(bin_fd);

    // Compile with c++ — include runtime header and compile runtime source
    char compile_cmd[1024];
    snprintf(compile_cmd, sizeof(compile_cmd),
             "c++ -std=c++17 -I%s -I%s -o %s %s %s 2>&1",
             rt_include_dir, rt_include_dir_root, bin_path, src_path, rt_src_path);

    if (debug) {
        printf("Compile command: %s\n", compile_cmd);
    }

    FILE* compile_pipe = popen(compile_cmd, "r");
    if (!compile_pipe) {
        remove(src_path);
        remove(bin_path);
        return result;
    }

    char compile_output[1024];
    String compile_errors;
    while (fgets(compile_output, sizeof(compile_output), compile_pipe)) {
        compile_errors.append(compile_output, static_cast<u32>(strlen(compile_output)));
    }
    int compile_status = pclose(compile_pipe);

    if (compile_status != 0) {
        fprintf(stderr, "[C Backend] C++ compilation failed:\n%s\n", compile_errors.c_str());
        if (debug) {
            fprintf(stderr, "=== Generated C++ ===\n%s\n", cpp_source.c_str());
        }
        remove(src_path);
        remove(bin_path);
        return result;
    }

    result.compile_success = true;

    // Run the binary
    char run_cmd[512];
    snprintf(run_cmd, sizeof(run_cmd), "%s 2>&1", bin_path);

    FILE* run_pipe = popen(run_cmd, "r");
    if (!run_pipe) {
        remove(src_path);
        remove(bin_path);
        return result;
    }

    char run_output[1024];
    while (fgets(run_output, sizeof(run_output), run_pipe)) {
        result.stdout_output.append(run_output, static_cast<u32>(strlen(run_output)));
    }
    int run_status = pclose(run_pipe);

    // pclose returns the exit status in the format of waitpid
    // WEXITSTATUS extracts the actual exit code
#ifdef _WIN32
    result.exit_code = run_status;
#else
    if (WIFEXITED(run_status)) {
        result.exit_code = WEXITSTATUS(run_status);
    } else {
        result.exit_code = -1;
    }
#endif

    result.run_success = true;

    // Cleanup temp files
    remove(src_path);
    remove(bin_path);

    return result;
}

Value compile_and_run(const char* source, StringView func_name, Span<Value> args, bool debug) {
    BumpAllocator allocator(8192);
    BCModule* module = compile(allocator, source, debug);
    if (!module) {
        return Value::make_null();
    }

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    if (!vm_call(&vm, func_name, args)) {
        vm_destroy(&vm);
        delete module;
        return Value::make_null();
    }

    Value result = vm_get_result(&vm);
    vm_destroy(&vm);
    delete module;
    return result;
}

// RAII helper for stdout capture using temporary file
class OutputCapture {
public:
    OutputCapture() : m_stdout_saved(-1), m_temp_file(nullptr) {
        m_temp_path[0] = '\0';

        // Generate temp file path
#ifdef _WIN32
        char temp_path[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        GetTempFileNameA(temp_path, "roxy", 0, m_temp_path);
#else
        const char* tmpdir = getenv("TMPDIR");
        if (!tmpdir) tmpdir = "/tmp";
        snprintf(m_temp_path, sizeof(m_temp_path), "%s/roxy_test_XXXXXX", tmpdir);
        int fd = mkstemp(m_temp_path);
        if (fd >= 0) close(fd);
#endif

        // Flush stdout before redirecting
        fflush(stdout);

        // Save original stdout
        m_stdout_saved = dup(fileno(stdout));

        // Redirect stdout to temp file
        m_temp_file = freopen(m_temp_path, "w", stdout);
    }

    ~OutputCapture() {
        restore();
        // Clean up temp file
        if (m_temp_path[0] != '\0') {
            remove(m_temp_path);
        }
    }

    void restore() {
        if (m_stdout_saved >= 0) {
            fflush(stdout);
            dup2(m_stdout_saved, fileno(stdout));
            close(m_stdout_saved);
            m_stdout_saved = -1;
            m_temp_file = nullptr;
        }
    }

    String read_output() {
        if (m_temp_path[0] == '\0') {
            return String();
        }

        FILE* f = fopen(m_temp_path, "rb");
        if (!f) {
            return String();
        }

        fseek(f, 0, SEEK_END);
        long len = ftell(f);
        fseek(f, 0, SEEK_SET);

        if (len <= 0) {
            fclose(f);
            return String();
        }

        String result;
        result.resize(static_cast<u32>(len));
        fread(result.data(), 1, static_cast<size_t>(len), f);
        fclose(f);
        return result;
    }

private:
    int m_stdout_saved;
    FILE* m_temp_file;
    char m_temp_path[256];
};

TestResult run_and_capture(const char* source, StringView func_name, Span<Value> args, bool debug) {
    TestResult result;
    result.success = false;
    result.value = 0;

    BumpAllocator allocator(8192);
    BCModule* module = compile(allocator, source, debug);
    if (!module) {
        return result;
    }

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    // Start capturing output
    OutputCapture capture;

    if (!vm_call(&vm, func_name, args)) {
        // Restore and capture output before cleanup
        capture.restore();
        result.stdout_output = capture.read_output();

        vm_destroy(&vm);
        delete module;
        return result;
    }

    // Restore stdout before reading captured output
    capture.restore();
    result.stdout_output = capture.read_output();

    Value vm_result = vm_get_result(&vm);
    result.value = vm_result.as_int;
    result.success = true;

    vm_destroy(&vm);
    delete module;
    return result;
}

} // namespace rx

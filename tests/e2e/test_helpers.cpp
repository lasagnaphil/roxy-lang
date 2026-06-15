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

String compile_to_cpp(const char* source, bool debug, const char* source_path) {
    BumpAllocator allocator(8192);
    IRModule* ir_module = compile_to_ir(allocator, source, debug);
    if (!ir_module) {
        return String();
    }

    CEmitterConfig config;
    config.emit_main_entry = true;
    if (source_path) config.source_path = String(source_path);
    CEmitter emitter(allocator, config);

    String output;
    emitter.emit_source(ir_module, output);

    if (debug) {
        printf("=== Generated C++ ===\n%s\n", output.c_str());
    }

    return output;
}

// Variant of compile_to_ir that uses an externally-supplied NativeRegistry
// (so the caller can pre-bind user functions). Must be called with a registry
// whose builtin natives are already registered.
static IRModule* compile_to_ir_with_registry(BumpAllocator& allocator,
                                             const char* source,
                                             NativeRegistry& registry,
                                             TypeEnv& type_env,
                                             bool debug) {
    u32 len = 0;
    while (source[len]) len++;

    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, type_env.types());

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();
    if (!program || parser.has_error()) return nullptr;

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
    if (!ir_module) return nullptr;

    coroutine_lower(ir_module, allocator, type_env);

    IRValidator validator;
    if (!validator.validate(ir_module)) {
        if (debug) fprintf(stderr, "IR validation failed: %s\n", validator.error());
        return nullptr;
    }

    return ir_module;
}

String compile_to_hpp(const char* source, bool debug) {
    BumpAllocator allocator(8192);
    IRModule* ir_module = compile_to_ir(allocator, source, debug);
    if (!ir_module) {
        return String();
    }

    CEmitterConfig config;
    config.emit_main_entry = false;  // header is for embedder use
    CEmitter emitter(allocator, config);

    String output;
    emitter.emit_header(ir_module, output);

    if (debug) {
        printf("=== Generated .hpp ===\n%s\n", output.c_str());
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

// The C-backend runtime (roxy_rt.cpp + slab_allocator.cpp + string_intern.cpp +
// platform vmem) is identical for every generated test program. Compiling those
// four translation units once per test process and linking the resulting object
// files — instead of recompiling them for every test case — is the dominant
// speedup for the C-backend suite (the per-test cost drops to one small
// generated TU plus a link).
struct RuntimeObjects {
    static constexpr int kCount = 4;
    char obj_paths[kCount][256];
    bool ok = false;
    String link_args;  // " <obj0> <obj1> ..." appended to the link command

    RuntimeObjects() {
        for (int i = 0; i < kCount; i++) obj_paths[i][0] = '\0';

        const char* project_root = get_project_root();
        if (!project_root) return;
        const char* tmpdir = getenv("TMPDIR");
        if (!tmpdir) tmpdir = "/tmp";

        char rt_include_dir[512], rt_include_dir_root[512];
        snprintf(rt_include_dir, sizeof(rt_include_dir), "%s/include/roxy/rt", project_root);
        snprintf(rt_include_dir_root, sizeof(rt_include_dir_root), "%s/include", project_root);

        char srcs[kCount][512];
        snprintf(srcs[0], sizeof(srcs[0]), "%s/src/roxy/rt/roxy_rt.cpp", project_root);
        snprintf(srcs[1], sizeof(srcs[1]), "%s/src/roxy/rt/slab_allocator.cpp", project_root);
        snprintf(srcs[2], sizeof(srcs[2]), "%s/src/roxy/rt/string_intern.cpp", project_root);
#ifdef _WIN32
        snprintf(srcs[3], sizeof(srcs[3]), "%s/src/roxy/rt/vmem_win32.cpp", project_root);
#else
        snprintf(srcs[3], sizeof(srcs[3]), "%s/src/roxy/rt/vmem_unix.cpp", project_root);
#endif

        for (int i = 0; i < kCount; i++) {
            snprintf(obj_paths[i], sizeof(obj_paths[i]), "%s/roxy_rt_obj_XXXXXX.o", tmpdir);
            int fd = mkstemps(obj_paths[i], 2);  // .o suffix
            if (fd < 0) { obj_paths[i][0] = '\0'; return; }
            close(fd);

            char cmd[2048];
            snprintf(cmd, sizeof(cmd),
                     "c++ -std=c++17 -I%s -I%s -c -o %s %s 2>&1",
                     rt_include_dir, rt_include_dir_root, obj_paths[i], srcs[i]);
            FILE* pipe = popen(cmd, "r");
            if (!pipe) return;
            String errs;
            char buf[1024];
            while (fgets(buf, sizeof(buf), pipe)) errs.append(buf, static_cast<u32>(strlen(buf)));
            if (pclose(pipe) != 0) {
                fprintf(stderr, "[C Backend] runtime precompile failed (%s):\n%s\n",
                        srcs[i], errs.c_str());
                return;
            }
            link_args.append(" ", 1);
            link_args.append(obj_paths[i], static_cast<u32>(strlen(obj_paths[i])));
        }
        ok = true;
    }

    ~RuntimeObjects() {
        for (int i = 0; i < kCount; i++)
            if (obj_paths[i][0] != '\0') remove(obj_paths[i]);
    }
};

// Lazily compiled on first use; reused by every C-backend compile in the process.
// doctest runs cases sequentially, so no synchronization is needed.
static const RuntimeObjects& runtime_objects() {
    static RuntimeObjects ro;
    return ro;
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

    // Runtime include paths (generated code does #include "roxy_rt.h").
    char rt_include_dir[512];       // For generated code: #include "roxy_rt.h"
    char rt_include_dir_root[512];  // For roxy_rt.h: #include "roxy/rt/..."
    snprintf(rt_include_dir, sizeof(rt_include_dir), "%s/include/roxy/rt", project_root);
    snprintf(rt_include_dir_root, sizeof(rt_include_dir_root), "%s/include", project_root);

    // Link the runtime objects compiled once per process, rather than
    // recompiling the runtime sources for every test.
    const RuntimeObjects& rt = runtime_objects();
    if (!rt.ok) {
        fprintf(stderr, "[C Backend] runtime objects unavailable\n");
        return result;
    }

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

    // Compile the generated source and link the prebuilt runtime objects.
    char compile_cmd[2048];
    snprintf(compile_cmd, sizeof(compile_cmd),
             "c++ -std=c++17 -I%s -I%s -o %s %s%s 2>&1",
             rt_include_dir, rt_include_dir_root, bin_path, src_path,
             rt.link_args.c_str());

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

CBackendResult compile_and_run_cpp_with_registry(const char* source,
                                                 NativeRegistry* registry,
                                                 const char* native_header_text,
                                                 const char* extra_cpp_text,
                                                 bool debug) {
    CBackendResult result;
    result.exit_code = -1;
    result.compile_success = false;
    result.run_success = false;

    if (!registry) return result;

    // Build the IR with the caller-supplied registry, then emit C++ with the
    // registry plumbed into CEmitterConfig so user-bound names dispatch to
    // direct calls.
    BumpAllocator allocator(16384);
    TypeEnv type_env(allocator);
    IRModule* ir_module = compile_to_ir_with_registry(allocator, source, *registry, type_env, debug);
    if (!ir_module) {
        fprintf(stderr, "[C Backend+Registry] IR build failed\n");
        return result;
    }

    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    // Optional inline native header — written to a temp file so the generated
    // .cpp can `#include` it via `native_include_paths`.
    char header_path[256];
    header_path[0] = '\0';
    if (native_header_text) {
        snprintf(header_path, sizeof(header_path), "%s/roxy_native_XXXXXX.h", tmpdir);
        int fd = mkstemps(header_path, 2);  // .h
        if (fd < 0) return result;
        size_t hlen = strlen(native_header_text);
        write(fd, native_header_text, hlen);
        close(fd);
    }

    // Optional extra .cpp — compiled as a separate translation unit and
    // linked into the binary. Used to verify cross-TU linkage against AOT
    // extern decls (the generated source carries only `extern` declarations
    // for user natives; this extra .cpp provides the definitions).
    char extra_cpp_path[256];
    extra_cpp_path[0] = '\0';
    if (extra_cpp_text) {
        snprintf(extra_cpp_path, sizeof(extra_cpp_path), "%s/roxy_native_extra_XXXXXX.cpp", tmpdir);
        int fd = mkstemps(extra_cpp_path, 4);  // .cpp
        if (fd < 0) {
            if (header_path[0] != '\0') remove(header_path);
            return result;
        }
        size_t clen = strlen(extra_cpp_text);
        write(fd, extra_cpp_text, clen);
        close(fd);
    }

    CEmitterConfig config;
    config.emit_main_entry = true;
    config.native_registry = registry;
    if (header_path[0] != '\0') {
        String include_path(header_path);
        config.native_include_paths.push_back(include_path);
    }

    CEmitter emitter(allocator, config);
    String cpp_source;
    emitter.emit_source(ir_module, cpp_source);

    if (debug) {
        printf("=== Generated C++ ===\n%s\n", cpp_source.c_str());
    }

    const char* project_root = get_project_root();
    if (!project_root) {
        if (header_path[0] != '\0') remove(header_path);
        return result;
    }

    char rt_include_dir[512];
    char rt_include_dir_root[512];
    snprintf(rt_include_dir, sizeof(rt_include_dir), "%s/include/roxy/rt", project_root);
    snprintf(rt_include_dir_root, sizeof(rt_include_dir_root), "%s/include", project_root);

    const RuntimeObjects& rt = runtime_objects();
    if (!rt.ok) {
        fprintf(stderr, "[C Backend+Registry] runtime objects unavailable\n");
        if (header_path[0] != '\0') remove(header_path);
        if (extra_cpp_path[0] != '\0') remove(extra_cpp_path);
        return result;
    }

    char src_path[256];
    char bin_path[256];
    snprintf(src_path, sizeof(src_path), "%s/roxy_cbackend_reg_XXXXXX.cpp", tmpdir);
    snprintf(bin_path, sizeof(bin_path), "%s/roxy_cbackend_reg_bin_XXXXXX", tmpdir);

    int src_fd = mkstemps(src_path, 4);
    if (src_fd < 0) {
        if (header_path[0] != '\0') remove(header_path);
        return result;
    }
    write(src_fd, cpp_source.data(), cpp_source.size());
    close(src_fd);

    int bin_fd = mkstemp(bin_path);
    if (bin_fd < 0) {
        remove(src_path);
        if (header_path[0] != '\0') remove(header_path);
        return result;
    }
    close(bin_fd);

    char compile_cmd[2560];
    snprintf(compile_cmd, sizeof(compile_cmd),
             "c++ -std=c++17 -I%s -I%s -o %s %s%s %s 2>&1",
             rt_include_dir, rt_include_dir_root, bin_path, src_path,
             rt.link_args.c_str(),
             extra_cpp_path[0] != '\0' ? extra_cpp_path : "");

    if (debug) printf("Compile command: %s\n", compile_cmd);

    bool compile_ok = false;
    {
        FILE* compile_pipe = popen(compile_cmd, "r");
        if (compile_pipe) {
            char buf[1024];
            String compile_errors;
            while (fgets(buf, sizeof(buf), compile_pipe)) {
                compile_errors.append(buf, static_cast<u32>(strlen(buf)));
            }
            int status = pclose(compile_pipe);
            if (status == 0) {
                compile_ok = true;
            } else {
                fprintf(stderr, "[C Backend+Registry] compile failed:\n%s\n",
                        compile_errors.c_str());
                if (debug) {
                    fprintf(stderr, "=== Generated C++ ===\n%s\n", cpp_source.c_str());
                }
            }
        }
    }

    if (compile_ok) {
        result.compile_success = true;
        char run_cmd[512];
        snprintf(run_cmd, sizeof(run_cmd), "%s 2>&1", bin_path);
        FILE* run_pipe = popen(run_cmd, "r");
        if (run_pipe) {
            char rbuf[1024];
            while (fgets(rbuf, sizeof(rbuf), run_pipe)) {
                result.stdout_output.append(rbuf, static_cast<u32>(strlen(rbuf)));
            }
            int run_status = pclose(run_pipe);
#ifdef _WIN32
            result.exit_code = run_status;
#else
            if (WIFEXITED(run_status)) result.exit_code = WEXITSTATUS(run_status);
            else result.exit_code = -1;
#endif
            result.run_success = true;
        }
    }

    remove(src_path);
    remove(bin_path);
    if (header_path[0] != '\0') remove(header_path);
    if (extra_cpp_path[0] != '\0') remove(extra_cpp_path);
    return result;
}

bool header_compiles(const char* source, bool debug) {
    String hpp_source = compile_to_hpp(source, debug);
    if (hpp_source.empty()) {
        if (debug) fprintf(stderr, "[Header] compile_to_hpp returned empty\n");
        return false;
    }

    const char* project_root = get_project_root();
    if (!project_root) {
        fprintf(stderr, "[Header] Failed to find project root\n");
        return false;
    }

    char rt_include_dir[512];
    char rt_include_dir_root[512];
    snprintf(rt_include_dir, sizeof(rt_include_dir), "%s/include/roxy/rt", project_root);
    snprintf(rt_include_dir_root, sizeof(rt_include_dir_root), "%s/include", project_root);

    const char* tmpdir = getenv("TMPDIR");
    if (!tmpdir) tmpdir = "/tmp";

    char hpp_path[256];
    char drv_path[256];
    char obj_path[256];
    snprintf(hpp_path, sizeof(hpp_path), "%s/roxy_header_XXXXXX.hpp", tmpdir);
    snprintf(drv_path, sizeof(drv_path), "%s/roxy_driver_XXXXXX.cpp", tmpdir);
    snprintf(obj_path, sizeof(obj_path), "%s/roxy_header_XXXXXX.o", tmpdir);

    int hpp_fd = mkstemps(hpp_path, 4);  // .hpp
    if (hpp_fd < 0) return false;
    write(hpp_fd, hpp_source.data(), hpp_source.size());
    close(hpp_fd);

    int drv_fd = mkstemps(drv_path, 4);  // .cpp
    if (drv_fd < 0) {
        remove(hpp_path);
        return false;
    }
    char driver[768];
    int dn = snprintf(driver, sizeof(driver),
                      "#include \"%s\"\nint main() { return 0; }\n", hpp_path);
    write(drv_fd, driver, static_cast<size_t>(dn));
    close(drv_fd);

    int obj_fd = mkstemps(obj_path, 2);  // .o
    if (obj_fd < 0) {
        remove(hpp_path);
        remove(drv_path);
        return false;
    }
    close(obj_fd);

    char compile_cmd[1536];
    snprintf(compile_cmd, sizeof(compile_cmd),
             "c++ -std=c++17 -I%s -I%s -c -o %s %s 2>&1",
             rt_include_dir, rt_include_dir_root, obj_path, drv_path);

    if (debug) printf("[Header] Compile command: %s\n", compile_cmd);

    FILE* pipe = popen(compile_cmd, "r");
    if (!pipe) {
        remove(hpp_path);
        remove(drv_path);
        remove(obj_path);
        return false;
    }

    char buf[1024];
    String errors;
    while (fgets(buf, sizeof(buf), pipe)) {
        errors.append(buf, static_cast<u32>(strlen(buf)));
    }
    int status = pclose(pipe);

    bool ok = (status == 0);
    if (!ok) {
        fprintf(stderr, "[Header] Compilation failed:\n%s\n", errors.c_str());
        if (debug) fprintf(stderr, "=== Generated .hpp ===\n%s\n", hpp_source.c_str());
    }

    remove(hpp_path);
    remove(drv_path);
    remove(obj_path);
    return ok;
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

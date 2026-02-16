#include "test_helpers.hpp"

#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstring>

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

    // Create type cache and registry
    TypeCache types(allocator);
    NativeRegistry registry(allocator, types);
    register_builtin_natives(registry);

    // Create module registry and register builtin module for prelude auto-import
    ModuleRegistry modules(allocator);
    modules.register_native_module(BUILTIN_MODULE_NAME, &registry, types);

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator, types, modules);
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

    IRBuilder ir_builder(allocator, analyzer.types(), registry, analyzer.symbols(), modules);
    IRModule* ir_module = ir_builder.build(program, synthetic_decls, &analyzer.generics());
    if (!ir_module) {
        return nullptr;
    }

    if (debug) {
        Vector<char> ir_str;
        ir_module_to_string(ir_module, ir_str);
        ir_str.push_back('\0');
        printf("=== IR ===\n%s\n", ir_str.data());
    }

    BytecodeBuilder bc_builder;
    bc_builder.set_registry(&registry);
    BCModule* module = bc_builder.build(ir_module);
    if (module) {
        // Register native functions with the module for runtime
        registry.apply_to_module(module);
    }
    return module;
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
        strcpy(m_temp_path, "/tmp/roxy_test_XXXXXX");
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

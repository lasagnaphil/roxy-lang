#include "test_helpers.hpp"

#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/lowering.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/interpreter.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>

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

    Lexer lexer(source, len);
    Parser parser(lexer, allocator);
    Program* program = parser.parse();

    if (!program || parser.has_error()) {
        return nullptr;
    }

    SemanticAnalyzer analyzer(allocator, &registry);
    if (!analyzer.analyze(program)) {
        return nullptr;
    }

    IRBuilder ir_builder(allocator, analyzer.types(), registry);
    IRModule* ir_module = ir_builder.build(program);
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
        // Generate temp file path
#ifdef _WIN32
        char temp_path[MAX_PATH];
        char temp_file[MAX_PATH];
        GetTempPathA(MAX_PATH, temp_path);
        GetTempFileNameA(temp_path, "roxy", 0, temp_file);
        m_temp_path = temp_file;
#else
        m_temp_path = "/tmp/roxy_test_XXXXXX";
        int fd = mkstemp(&m_temp_path[0]);
        if (fd >= 0) close(fd);
#endif

        // Flush stdout before redirecting
        fflush(stdout);

        // Save original stdout
        m_stdout_saved = dup(fileno(stdout));

        // Redirect stdout to temp file
        m_temp_file = freopen(m_temp_path.c_str(), "w", stdout);
    }

    ~OutputCapture() {
        restore();
        // Clean up temp file
        if (!m_temp_path.empty()) {
            remove(m_temp_path.c_str());
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

    std::string read_output() {
        if (m_temp_path.empty()) {
            return "";
        }

        std::ifstream file(m_temp_path);
        if (!file.is_open()) {
            return "";
        }

        std::stringstream buffer;
        buffer << file.rdbuf();
        return buffer.str();
    }

private:
    int m_stdout_saved;
    FILE* m_temp_file;
    std::string m_temp_path;
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

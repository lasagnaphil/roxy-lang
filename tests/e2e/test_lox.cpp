// End-to-end driver for the Lox interpreter (implemented in Roxy under
// examples/lox/). Loads the project's .roxy files from disk, compiles them
// as modules, and runs main() from examples/lox/test.roxy. The Roxy test
// harness returns 0 on success and non-zero if any Lox-side assertion failed.

#include "roxy/core/doctest/doctest.h"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/file.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/compiler.hpp"
#include "roxy/vm/vm.hpp"
#include "roxy/vm/value.hpp"

#include <cstdio>
#include <cstring>
#include <string>

namespace rx {

static std::string project_root_path() {
#ifdef ROXY_PROJECT_ROOT
    return std::string(ROXY_PROJECT_ROOT);
#else
    return std::string(".");
#endif
}

struct LoxSourceFile {
    const char* module_name;
    const char* rel_path;
};

TEST_CASE("E2E - Lox: full interpreter (from examples/lox/test.roxy)") {
    const LoxSourceFile files[] = {
        {"tokens",      "/examples/lox/tokens.roxy"},
        {"scanner",     "/examples/lox/scanner.roxy"},
        {"value",       "/examples/lox/value.roxy"},
        {"ast",         "/examples/lox/ast.roxy"},
        {"parser",      "/examples/lox/parser.roxy"},
        {"interpreter", "/examples/lox/interpreter.roxy"},
        {"resolver",    "/examples/lox/resolver.roxy"},
        {"test",        "/examples/lox/test.roxy"},
    };

    // Read each .roxy file off disk.
    std::string root = project_root_path();
    Vector<Vector<u8>> buffers;
    buffers.resize(sizeof(files) / sizeof(files[0]));

    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        std::string path = root + files[i].rel_path;
        bool ok = read_file_to_buf(path.c_str(), buffers[i]);
        INFO("Failed to read ", path);
        REQUIRE(ok);
    }

    // Compile all three modules together.
    BumpAllocator allocator(65536);
    Compiler compiler(allocator);
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        const char* src = reinterpret_cast<const char*>(buffers[i].data());
        // Buffer is null-terminated by read_file_to_buf; length excludes the terminator.
        u32 len = static_cast<u32>(buffers[i].size() - 1);
        compiler.add_source(files[i].module_name, src, len);
    }

    BCModule* module = compiler.compile();
    if (!module) {
        fprintf(stderr, "Lox compile errors:\n");
        for (const char* err : compiler.errors()) {
            fprintf(stderr, "  %s\n", err);
        }
    }
    REQUIRE(module != nullptr);

    // Run main() from the "test" module.
    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, module);

    bool ok = vm_call(&vm, "main", {});
    if (!ok) {
        fprintf(stderr, "Lox runtime error: %s\n", vm.error ? vm.error : "(unknown)");
    }
    REQUIRE(ok);

    Value result = vm_get_result(&vm);
    CHECK_MESSAGE(result.as_int == 0,
                  "examples/lox/test.roxy returned non-zero; see FAIL lines above");

    vm_destroy(&vm);
    delete module;
}

} // namespace rx

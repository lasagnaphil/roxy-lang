// Structure-aware fuzz harness body: fuzzer bytes drive the structural
// generator (tests/fuzz/gen), whose output is a valid-by-construction Roxy
// program; the program is then compiled through the full pipeline (parser,
// sema, IR builder, optimizer, lowering) and executed on the VM.
//
// Unlike the byte-level harnesses, the input is never parsed as source text —
// it is an entropy stream, so libFuzzer's mutations mutate *program structure*
// and coverage feedback steers generation toward unexplored compiler paths.
// See docs/internals/fuzzer.md ("Roadmap: structure-aware fuzzing").
//
// Oracles:
//  - Compiler must accept: a rejected program means the generator's model of
//    the language and the compiler disagree — a bug in one of them. We abort
//    so libFuzzer saves the reproducer.
//  - Compile/run must not crash, hang, or trip UBSan: generated programs are
//    terminating by construction, so non-termination is a compiler/VM bug.

#include "fuzz_targets.hpp"
#include "generator.hpp"

#include "roxy/compiler/compiler.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/vm/vm.hpp"

#include <cstdio>
#include <cstdlib>

namespace rx::fuzz {

void fuzz_one_structured(const uint8_t* data, size_t size) {
    gen::Entropy entropy(data, size);
    gen::GenConfig config = gen::GenConfig::fuzz_default();
    config.num_modules = 1 + entropy.range(3);  // exercise toposort + linking
    gen::GeneratedProgram program = gen::generate_program(entropy, config);

    BumpAllocator allocator(1 << 16);
    Compiler compiler(allocator);
    for (const auto& module : program.modules) {
        compiler.add_source(StringView{module.name.c_str(), static_cast<u32>(module.name.size())},
                            module.source.c_str(), static_cast<u32>(module.source.size()));
    }

    BCModule* bc_module = compiler.compile();
    if (bc_module == nullptr || compiler.has_errors()) {
        fprintf(stderr, "structured fuzz: compiler rejected a generated program:\n");
        for (const char* error : compiler.errors()) {
            fprintf(stderr, "  error: %s\n", error);
        }
        for (const auto& module : program.modules) {
            fprintf(stderr, "── %s.roxy ──\n%s\n", module.name.c_str(), module.source.c_str());
        }
        abort();
    }

    RoxyVM vm;
    vm_init(&vm);
    vm_load_module(&vm, bc_module);
    bool run_ok = vm_call(&vm, "main", {});
    if (!run_ok) {
        fprintf(stderr, "structured fuzz: VM failed running a generated program\n");
        abort();
    }
    vm_destroy(&vm);
    delete bc_module;
}

} // namespace rx::fuzz

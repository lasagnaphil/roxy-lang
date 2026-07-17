#include "roxy/core/doctest/doctest.h"

#include "generator.hpp"

#include "roxy/compiler/compiler.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/vm/vm.hpp"

#include <string>

// Always-on regression suite for the structural generator (tests/fuzz/gen).
//
// The generator's contract is that every program it emits is valid by
// construction: it must compile (parse, sema, IR, lowering) and terminate on
// the VM. These fixed-seed replays enforce that contract on every test run —
// a failure means the generator's model of Roxy and the compiler disagree,
// which is a bug in one of them (and either way worth catching immediately).
//
// This is the same generator the fuzz_structured libFuzzer target drives with
// mutated bytes, and the roxy_gen CLI drives with a seed to emit benchmark
// corpora, so this suite also keeps both of those from bit-rotting.

namespace {

struct RunOutcome {
    bool compiled = false;
    bool ran = false;
    rx::i64 result = 0;
    std::string errors;
};

RunOutcome compile_and_run_generated(const rx::gen::GeneratedProgram& program) {
    RunOutcome outcome;
    rx::BumpAllocator allocator(1 << 16);
    rx::Compiler compiler(allocator);
    for (const auto& module : program.modules) {
        compiler.add_source(
            rx::StringView{module.name.c_str(), static_cast<rx::u32>(module.name.size())},
            module.source.c_str(), static_cast<rx::u32>(module.source.size()));
    }

    rx::BCModule* bc_module = compiler.compile();
    if (bc_module == nullptr || compiler.has_errors()) {
        for (const char* error : compiler.errors()) {
            outcome.errors += error;
            outcome.errors += '\n';
        }
        return outcome;
    }
    outcome.compiled = true;

    rx::RoxyVM vm;
    rx::vm_init(&vm);
    rx::vm_load_module(&vm, bc_module);
    outcome.ran = rx::vm_call(&vm, "main", {});
    if (outcome.ran) outcome.result = rx::vm_get_result(&vm).as_int;
    rx::vm_destroy(&vm);
    delete bc_module;
    return outcome;
}

std::string dump_program(const rx::gen::GeneratedProgram& program) {
    std::string text;
    for (const auto& module : program.modules) {
        text += "── " + module.name + ".roxy ──\n" + module.source + "\n";
    }
    return text;
}

void check_seed_range(uint64_t seed_begin, uint64_t seed_end, const rx::gen::GenConfig& config) {
    for (uint64_t seed = seed_begin; seed < seed_end; seed++) {
        CAPTURE(seed);
        rx::gen::Entropy entropy(seed);
        rx::gen::GeneratedProgram program = rx::gen::generate_program(entropy, config);
        RunOutcome outcome = compile_and_run_generated(program);
        if (!outcome.compiled || !outcome.ran) {
            // Dump the offending program so the disagreement is reproducible
            // straight from the test log.
            MESSAGE("generated program (seed ", seed, "):\n", dump_program(program),
                    "compiler errors:\n", outcome.errors);
        }
        REQUIRE(outcome.compiled);
        REQUIRE(outcome.ran);
    }
}

} // namespace

TEST_SUITE("Structured Gen") {
    TEST_CASE("fixed seeds compile and run: single module") {
        rx::gen::GenConfig config = rx::gen::GenConfig::fuzz_default();
        check_seed_range(1, 26, config);
    }

    TEST_CASE("fixed seeds compile and run: multi-module") {
        rx::gen::GenConfig config = rx::gen::GenConfig::fuzz_default();
        config.num_modules = 4;
        check_seed_range(100, 110, config);
    }

    TEST_CASE("fixed seeds compile and run: benchmark shape") {
        // The corpus roxy_gen emits, at small scale (prints disabled so the
        // test run stays quiet).
        rx::gen::GenConfig config = rx::gen::GenConfig::benchmark_default();
        config.num_modules = 6;
        config.allow_print = false;
        check_seed_range(200, 205, config);
    }

    TEST_CASE("byte-buffer entropy: dry buffer degrades to a minimal program") {
        // Byte mode is what fuzz_structured uses; an (almost) empty buffer
        // must still yield a compiling, running program.
        const uint8_t bytes[] = {0x7f, 0x03};
        rx::gen::Entropy entropy(bytes, sizeof(bytes));
        rx::gen::GeneratedProgram program =
            rx::gen::generate_program(entropy, rx::gen::GenConfig::fuzz_default());
        RunOutcome outcome = compile_and_run_generated(program);
        if (!outcome.compiled || !outcome.ran) {
            MESSAGE("generated program:\n", dump_program(program), "errors:\n", outcome.errors);
        }
        REQUIRE(outcome.compiled);
        REQUIRE(outcome.ran);
    }

    TEST_CASE("generation is deterministic per seed") {
        rx::gen::GenConfig config = rx::gen::GenConfig::fuzz_default();
        config.num_modules = 3;
        rx::gen::Entropy entropy_a(42);
        rx::gen::Entropy entropy_b(42);
        rx::gen::GeneratedProgram program_a = rx::gen::generate_program(entropy_a, config);
        rx::gen::GeneratedProgram program_b = rx::gen::generate_program(entropy_b, config);
        REQUIRE(program_a.modules.size() == program_b.modules.size());
        for (size_t i = 0; i < program_a.modules.size(); i++) {
            CHECK(program_a.modules[i].name == program_b.modules[i].name);
            CHECK(program_a.modules[i].source == program_b.modules[i].source);
        }

        rx::gen::Entropy entropy_c(43);
        rx::gen::GeneratedProgram program_c = rx::gen::generate_program(entropy_c, config);
        bool any_difference = program_a.modules.size() != program_c.modules.size();
        for (size_t i = 0; !any_difference && i < program_a.modules.size(); i++) {
            any_difference = program_a.modules[i].source != program_c.modules[i].source;
        }
        CHECK(any_difference);
    }
}

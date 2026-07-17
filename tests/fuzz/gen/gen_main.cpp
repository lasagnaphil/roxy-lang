// roxy_gen — benchmark-corpus generator CLI.
//
// Emits a reproducible (seeded), multi-module Roxy project of configurable
// size, valid by construction, for compile-time profiling:
//
//   roxy_gen --seed=42 --modules=200 --out=/tmp/corpus_200
//   roxy --time /tmp/corpus_200/main.roxy
//
// Generate the same project at several --modules sizes and plot per-phase
// compile time against LOC to spot super-linear behavior (see
// docs/internals/profiling.md).

#include "generator.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

static void print_usage() {
    fprintf(stderr, "Usage: roxy_gen [options]\n");
    fprintf(stderr, "\nOptions:\n");
    fprintf(stderr, "  --seed=N     RNG seed (default 1); same seed -> identical corpus\n");
    fprintf(stderr, "  --modules=N  number of modules incl. main (default 50)\n");
    fprintf(stderr, "  --funcs=N    max free functions per module (default 14)\n");
    fprintf(stderr, "  --out=DIR    write <module>.roxy files into DIR (created if needed)\n");
    fprintf(stderr, "  --print      dump all modules to stdout instead of writing files\n");
    fprintf(stderr, "\nCompile the result with: roxy --time DIR/main.roxy\n");
}

int main(int argc, char** argv) {
    uint64_t seed = 1;
    const char* out_dir = nullptr;
    bool print_to_stdout = false;
    rx::gen::GenConfig config = rx::gen::GenConfig::benchmark_default();

    for (int i = 1; i < argc; i++) {
        if (strncmp(argv[i], "--seed=", 7) == 0) {
            seed = strtoull(argv[i] + 7, nullptr, 10);
        } else if (strncmp(argv[i], "--modules=", 10) == 0) {
            config.num_modules = static_cast<uint32_t>(strtoul(argv[i] + 10, nullptr, 10));
            if (config.num_modules == 0) config.num_modules = 1;
        } else if (strncmp(argv[i], "--funcs=", 8) == 0) {
            config.max_funcs_per_module = static_cast<uint32_t>(strtoul(argv[i] + 8, nullptr, 10));
            if (config.max_funcs_per_module < config.min_funcs_per_module) {
                config.min_funcs_per_module = config.max_funcs_per_module;
            }
        } else if (strncmp(argv[i], "--out=", 6) == 0) {
            out_dir = argv[i] + 6;
        } else if (strcmp(argv[i], "--print") == 0) {
            print_to_stdout = true;
        } else {
            fprintf(stderr, "Unknown option: %s\n\n", argv[i]);
            print_usage();
            return 1;
        }
    }
    if (!out_dir && !print_to_stdout) {
        print_usage();
        return 1;
    }

    rx::gen::Entropy entropy(seed);
    rx::gen::GeneratedProgram program = rx::gen::generate_program(entropy, config);

    if (print_to_stdout) {
        for (const auto& module : program.modules) {
            printf("// ══════ %s.roxy ══════\n%s\n", module.name.c_str(), module.source.c_str());
        }
    } else {
        std::error_code ec;
        std::filesystem::create_directories(out_dir, ec);
        if (ec) {
            fprintf(stderr, "Error: could not create directory '%s': %s\n", out_dir,
                    ec.message().c_str());
            return 1;
        }
        for (const auto& module : program.modules) {
            std::string path = std::string(out_dir) + "/" + module.name + ".roxy";
            std::ofstream file(path, std::ios::binary);
            if (!file) {
                fprintf(stderr, "Error: could not write '%s'\n", path.c_str());
                return 1;
            }
            file << module.source;
        }
    }

    const rx::gen::GenStats& stats = program.stats;
    fprintf(stderr, "Generated: %u modules, %u functions, %u structs, %u enums, %u lines (seed %llu)\n",
            stats.modules, stats.functions, stats.structs, stats.enums, stats.lines,
            static_cast<unsigned long long>(seed));
    if (out_dir) {
        fprintf(stderr, "Compile with: roxy --time %s/main.roxy\n", out_dir);
    }
    return 0;
}

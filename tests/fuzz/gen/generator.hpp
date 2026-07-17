#pragma once

#include "entropy.hpp"

#include <cstdint>
#include <string>
#include <vector>

// Structure-aware (type-directed) Roxy program generator.
//
// Generates multi-module programs that are **valid by construction**: every
// emitted program must lex, parse, pass semantic analysis, compile, and — when
// run — terminate. Any generated program the compiler rejects is a bug in
// either the generator's model of the language or the compiler itself (the
// disagreement is the finding). See docs/internals/fuzzer.md — this is the
// "type-directed generation" stage of the structure-aware roadmap.
//
// Validity is guaranteed by:
//  - Scoping: expressions only reference in-scope, initialized variables;
//    all names are globally unique (no shadowing, no keyword collisions).
//  - Types: expression generation is directed by a wanted type, mirroring the
//    checker in reverse (the Csmith approach).
//  - Termination: all loops are constant-bounded; functions may only call
//    functions generated before them, so the call graph is acyclic.
//  - No runtime traps: integer division only by nonzero literals; no
//    float->int casts (out-of-range conversion semantics not pinned yet);
//    no uniq/ref/weak in v1, so no move/borrow states to model.
//
// Two consumers share this generator via `Entropy`'s two modes:
//  - roxy_gen CLI: seeded PRNG -> reproducible benchmark corpora at any scale
//    (compile-time profiling with `roxy --time`).
//  - fuzz_structured: libFuzzer bytes -> coverage-guided program mutation,
//    reaching sema/IR/lowering/VM with valid programs.
namespace rx::gen {

struct GenConfig {
    uint32_t num_modules = 1;           // >= 1; the last module is "main"
    uint32_t min_funcs_per_module = 2;  // free functions, not counting methods
    uint32_t max_funcs_per_module = 6;
    uint32_t max_structs_per_module = 3;
    uint32_t max_enums_per_module = 2;
    uint32_t max_methods_per_struct = 2;
    uint32_t max_stmts_per_block = 5;
    uint32_t max_expr_depth = 3;
    uint32_t max_params = 3;
    uint32_t max_block_depth = 3;       // nesting cap for compound statements
    // Per-function statement budget across all nested blocks (realism: real
    // functions rarely exceed ~50 statements; corpora scale by breadth).
    uint32_t max_stmts_per_function = 30;
    // Per-function *dynamic* cost budget. Loops multiply through acyclic call
    // chains (f loops 8x calling g, which loops 8x calling h, ...), so without
    // a compositional bound a generated program terminates but can take
    // astronomically long to run. Each function's estimated cost (statements x
    // enclosing loop trip counts, callee costs included) is tracked, and call
    // sites are only generated where callee_cost x loop_multiplier still fits
    // this budget. Keeps every generated program's runtime in the millisecond
    // range on the VM.
    uint64_t max_dynamic_cost = 20000;
    bool allow_print = false;           // emit a final print() in main (stdout!)
    bool use_generics = true;           // generic fn/struct + instantiations
    bool use_methods = true;
    bool use_fstrings = true;
    bool use_cross_module = true;       // import / from-import + qualified calls

    // Small program shapes for per-input fuzz iterations and CI regression.
    static GenConfig fuzz_default();
    // Meatier per-module content for compile-time benchmark corpora.
    static GenConfig benchmark_default();
};

struct GeneratedModule {
    std::string name;    // module name == file basename (e.g. "m3_barkel")
    std::string source;
};

struct GenStats {
    uint32_t modules = 0;
    uint32_t functions = 0;  // free functions + methods + generic functions
    uint32_t structs = 0;
    uint32_t enums = 0;
    uint32_t lines = 0;
};

struct GeneratedProgram {
    std::vector<GeneratedModule> modules;  // modules.back() is always "main"
    GenStats stats;
};

GeneratedProgram generate_program(Entropy& entropy, const GenConfig& config);

} // namespace rx::gen

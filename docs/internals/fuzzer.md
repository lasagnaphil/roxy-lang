# Fuzzing

> **Status:** Byte-level coverage-guided fuzzing of the front-end (lexer, parser,
> LSP error-recovering parser) and a **structure-aware (type-directed) generator**
> (stages 1–2 of the staged plan below) are **implemented**. The generator reaches
> sema, the IR builder, the optimizer, lowering, and the VM with
> valid-by-construction programs, and doubles as the benchmark-corpus generator
> (`roxy_gen`). Move-state/lifetime modeling and the VM-vs-C differential oracle
> remain future work (see "Roadmap: structure-aware fuzzing").

The build/run quickstart — toolchain, `ENABLE_FUZZERS` flags, targets, libFuzzer
options, crash triage — lives in [`tests/fuzz/README.md`](../../tests/fuzz/README.md).
This document covers how it works and where it is going.

The oracles are the generic libFuzzer ones: a **crash** (SIGSEGV/`abort`/failed
`assert`), a **UBSan** abort (ASan too, when enabled), a **hang** (`-timeout`), or
an **OOM** (`-rss_limit_mb`). Coverage guidance evolves inputs toward unexplored
edges, so seeding from real `.roxy` files gives a large head start.

## Byte-level harnesses

One harness body per component (`tests/fuzz/fuzz_one_*.cpp`, declared in
`fuzz_targets.hpp`), so each fuzz executable links only the library it exercises;
the `LLVMFuzzerTestOneInput` entry points just forward to them. The LSP parser is
the highest-value byte-level target: it is explicitly built to consume
arbitrary/malformed input and must *never* crash or hang.

### Input buffer design (`detail::SourceBuffer`)

- **Exact-size copy plus one terminating `\0`.** The lexer's `peek()` relies on a
  `\0` sentinel at `length`, exactly as production source provides, so the buffer
  is `size + 1` bytes with a NUL at the end. Anything *past* the sentinel is still
  memory the harness does not own, so a fresh heap allocation per input keeps a
  genuine over-read a real out-of-bounds access a sanitizer can catch.
- **Fresh `BumpAllocator` per input**, so no state leaks between inputs and a
  saved reproducer replays deterministically.

Inputs larger than `UINT32_MAX` are rejected (the lexer's offsets are `u32`; input
size is not the property under test).

### Regression replay (`tests/unit/test_fuzz_regression.cpp`)

The `Fuzz Regression` doctest suite replays *fixed, known* inputs — the seed corpus
(`tests/fuzz/corpus/`), every `examples/*.roxy`, and inline adversarial cases —
through all three byte-level harness bodies on every normal `roxy_tests` run. It
needs no fuzzer toolchain. Its purpose is not discovery but to keep
found-and-fixed crashes fixed and stop the harnesses from bit-rotting; it calls
the exact `fuzz_one_*` functions the libFuzzer targets use, so the two cannot
drift.

> The harnesses guarantee bounded work per input, so the replay is fast. **Never
> add a slow or OOM reproducer to `tests/fuzz/corpus/`** — the replay has no
> resource cap and would hang or exhaust memory on every test run. Such
> reproducers are tracked in `TODO.md` and kept out of the corpus.

Worth knowing: the replay itself is a discovery channel — the LSP parser's two
forward-progress hangs surfaced there (a valid example simply hung; see
[lsp-server.md](lsp-server.md) → "Forward-progress invariant"), while the
coverage-guided campaign surfaced lexer UB and an LSP-parser OOM that is still
**open** (`TODO.md`).

## Structural generator

`tests/fuzz/gen/` holds a **type-directed Roxy program generator** whose output
is valid by construction: every emitted program must lex, parse, pass sema,
compile, and terminate in bounded time when run. A program the compiler rejects
(or that crashes any pass) is a bug in either the generator's model of the
language or the compiler — the disagreement is the finding. Validity is
guaranteed by construction:

- **Scoping** — expressions only reference in-scope variables; all names are
  globally unique (varied-length syllable names + counter, so identifier
  interning/hashing stays realistic).
- **Types** — `gen_expr(want, depth)` runs the checker in reverse (the Csmith
  approach): literals/vars/calls/fields/methods/casts are chosen to produce
  exactly the wanted type, under a depth budget.
- **Termination** — all loops are constant-bounded and functions only call
  functions generated before them (acyclic call graph).
- **Bounded runtime** — loops multiply through call chains, so termination
  alone permits astronomical runtimes; a compositional dynamic-cost model
  (each function's estimated cost includes its callees; call sites are only
  generated where `callee_cost x loop_multiplier` fits `max_dynamic_cost`)
  keeps every program's VM runtime in the millisecond range.
- **No runtime traps** — integer division only by nonzero literals; no
  float→int casts (conversion semantics not pinned yet); no `uniq`/`ref`/`weak`
  yet (that is the roadmap's stage 3).

Coverage generated today: multi-module programs (imports/from-imports with a
low-index-biased DAG, `pub` visibility, qualified cross-module calls), enums +
exhaustive/else `when`, structs (nested fields, methods with `self`
reads/writes), generics (identity functions with explicit/inferred type args,
`Box<T>`-style struct instantiation via field inference), f-strings, string
ops, and bounded `for`/`while`/`if` control flow.

One generator, driven through the dual-mode `Entropy` source (`entropy.hpp`):

- **Seeded PRNG** → `roxy_gen` CLI emits reproducible benchmark corpora at any
  scale (see `profiling.md` → "Benchmark corpora at scale"), and the always-on
  `Structured Gen` doctest suite (`tests/unit/test_structured_gen.cpp`) replays
  fixed seeds (compile + run on the VM) so generator-vs-compiler drift is caught
  on every test run.
- **Fuzzer bytes** → the `fuzz_structured` libFuzzer target treats the input as
  an entropy stream; mutating bytes mutates *program structure*, and coverage
  feedback steers generation into unexplored compiler paths. A dry buffer
  degrades to a minimal program (choice index 0 is always terminal), so short
  inputs stay valid. This plays the role libprotobuf-mutator would, without a
  protobuf schema.

## Roadmap: structure-aware fuzzing

### The validity spectrum

How valid you generate determines which passes you reach:

| Validity level | Must respect | Reaches |
|---|---|---|
| Lexically valid | token rules | lexer |
| Syntactically valid | the grammar | parser, sema's *error/reject* paths |
| Name-resolved | scoping (only in-scope symbols) | symbol resolution |
| Type-correct | the type system | type checker, IR builder, lowering |
| Lifetime-correct | `uniq`/`ref`/`weak`, move-state, `when` exhaustiveness | VM, C backend, drop plans, RAII codegen |

The generator is at "type-correct" today.

### Remaining Roxy-specific constraints

- **`uniq`/`ref`/`weak` + move-state** — the hard, high-value one. After
  consuming a `uniq` value, the generator must drop it from the usable pool or it
  emits a use-after-move and is rejected before reaching the IR builder. Modeling
  this is exactly the `LifetimeChecker` invariant, and it is where lifetime-,
  drop-, and RAII-codegen bugs hide.
- **Tagged unions** — generate the enum and its variants first, then emit
  variant-field structs and matching `when` statements.
- **Generics + trait bounds** — only instantiate `<T: Add>` with types that
  implement `Add`; pick call args of the instantiated types.

### Richer oracles

- **Differential testing** — compile the *same* generated program with the VM
  (`compile_and_run`) and the C backend (`compile_and_run_cpp`); the results must
  match. Any divergence is a miscompilation in one backend — the class of bug unit
  tests miss most. This is the single highest-value next oracle (the generated
  `main` already prints a checksum as its observable output).
- **Valid-program-shouldn't-crash-the-compiler** — already active: any
  well-typed program that makes sema/IR/lowering `assert` is a compiler bug. It
  found five compiler bugs (lowering, `ir_optimize`, string self-assignment,
  register overflow) in the generator's first campaign.
- **Round-trip stability** — with an AST→source printer, `parse(unparse(ast))`
  should be structurally identical to `ast`, and `unparse` idempotent.

### Staged plan

1. ~~**Syntactic generator**~~ — subsumed: the implemented generator went
   straight to type-directed output.
2. **Scoping + type-directed generation** — **implemented** (above). Next step:
   the VM-vs-C differential oracle.
3. **Move-state / lifetime modeling** — reach the RAII/drop/codegen paths that are
   hardest to cover with hand-written tests (`uniq`/`ref`/`weak`, use-after-move
   avoidance, exceptions, closures, coroutines in generated code).

**Caveat:** a semantically-valid generator is a *second implementation* of Roxy's
type-and-lifetime rules. Disagreements with the compiler produce false positives
(generator thinks a program is valid, compiler rejects it) and are a maintenance
cost — but the disagreements are themselves often bugs in one side or the other.

## Files

`tests/fuzz/` holds the harness bodies and entry points, `gen/` (generator,
`entropy.hpp`, the `roxy_gen` CLI in `gen_main.cpp`), and `corpus/` (seed corpus +
fixed crash reproducers). The generator library and `roxy_gen` CLI build in every
configuration (no fuzzer toolchain needed).

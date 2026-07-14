# Fuzzing

> **Status:** Byte-level coverage-guided fuzzing of the front-end (lexer, parser,
> LSP error-recovering parser) is **implemented** — libFuzzer targets in
> `tests/fuzz/` plus an always-on `Fuzz Regression` doctest replay. Structure-aware
> (grammar/type-directed) fuzzing to reach sema/IR/lowering/codegen is a **design
> plan** (see Roadmap) — not yet built.

Fuzzing feeds a large volume of automatically-generated inputs to a component and
watches for any that make it misbehave (crash, hang, trip a sanitizer, exhaust
memory). The front-end is the natural first target: it consumes untrusted text,
and its failure modes — non-termination, buffer over-reads, integer overflow —
are exactly what the generic oracles below detect. Practical build/run commands
live in `tests/fuzz/README.md`; this document covers how it works and where it is
going.

## How coverage-guided fuzzing works

Three pieces:

- **Target** — a function taking raw bytes. Ours is the libFuzzer entry point
  `LLVMFuzzerTestOneInput(const uint8_t* data, size_t size)`, which the engine
  calls millions of times, each with a different input. It forwards to a shared
  harness body that runs one component over the bytes.
- **Oracle** — how "bad" is decided. There are no hand-written
  `assert(result == expected)` checks (there is no known-good output for random
  input). Instead a bug manifests generically: the process **crashes**
  (SIGSEGV/`abort`), a **sanitizer** aborts (UBSan on undefined behavior; ASan on
  a memory error, when enabled), the input exceeds `-timeout` (a **hang**), or it
  exceeds `-rss_limit_mb` (an **OOM**). libFuzzer saves the offending input as
  `crash-*` / `timeout-*` / `oom-*` and stops.
- **Engine** — the loop. Pure random bytes are hopeless for a parser (they die in
  the lexer), so libFuzzer is *coverage-guided*: the compiler instruments every
  branch (SanitizerCoverage), and libFuzzer keeps a corpus of inputs that
  collectively maximize edge coverage. Each iteration mutates a corpus entry, runs
  it, and — if it hit a **new** edge — adds it back to the corpus. The search
  evolves inputs toward unexplored code, so seeding from real `.roxy` files gives
  it a large head start over starting from empty.

```
Source text ── fuzz input ──▶ Lexer / Parser / LspParser
                                     │
                     coverage feedback (new edges → keep input)
                                     │
                          crash / hang / UBSan / OOM  → save reproducer
```

## Current implementation (byte-level)

### Harnesses

One body per component, so each fuzz executable links only the library it
exercises (lexer-only coverage for the lexer target, etc.). Bodies are declared
in `tests/fuzz/fuzz_targets.hpp` and defined in `fuzz_one_{lexer,parser,lsp_parser}.cpp`:

| Target | Component | Body |
|---|---|---|
| `fuzz_lexer` | `rx::Lexer` — tokenize to `Eof` | `fuzz_one_lexer.cpp` |
| `fuzz_parser` | `rx::Parser` — fail-fast AST parse | `fuzz_one_parser.cpp` |
| `fuzz_lsp_parser` | `rx::LspParser` — error-recovering CST parse | `fuzz_one_lsp_parser.cpp` |

The `LLVMFuzzerTestOneInput` entry points (`fuzz_{lexer,parser,lsp_parser}.cpp`)
are three lines each — they just call the matching body. The LSP parser is the
highest-value target: it is explicitly built to consume arbitrary/malformed input
and must *never* crash or hang, so any input that breaks that is a bug.

### Input buffer design (`detail::SourceBuffer`)

Two deliberate choices in the shared harness header:

- **Exact-size, non-null-terminated copy.** Production source is always
  `\0`-terminated; the fuzz input deliberately is not. So if the lexer ever reads
  past `length` (the unchecked `advance()`), it touches memory it does not own — a
  real out-of-bounds read a sanitizer can catch — instead of harmlessly hitting a
  sentinel `\0`. A fresh exact-size heap allocation per input maximizes the odds
  such an over-read faults.
- **Fresh `BumpAllocator` per input.** All AST/CST nodes for one input live in an
  allocator destroyed at the end of the call, so no state leaks between inputs and
  a saved reproducer replays deterministically.

Inputs larger than `UINT32_MAX` are rejected (the lexer's offsets are `u32`; input
size is not the property under test).

### Regression replay (`tests/unit/test_fuzz_regression.cpp`)

A plain doctest suite (`Fuzz Regression`) that replays *fixed, known* inputs — the
seed corpus (`tests/fuzz/corpus/`), every `examples/*.roxy`, and inline adversarial
cases — through all three harness bodies on every normal `roxy_tests` run. It needs
no fuzzer toolchain and runs in-sandbox. Its purpose is not discovery but to keep
found-and-fixed crashes fixed and stop the harnesses from bit-rotting. It shares
the exact `fuzz_one_*` functions the libFuzzer targets use, so the two cannot
drift.

> The harnesses guarantee bounded work per input, so the replay is fast. **Never
> add a slow or OOM reproducer to `tests/fuzz/corpus/`** — the replay has no
> resource cap and would hang or exhaust memory on every test run. Such
> reproducers are tracked in `TODO.md` and kept out of the corpus.

### Build integration

The `ENABLE_FUZZERS` CMake option (off by default) wires it up:

- `-fsanitize=fuzzer-no-link,undefined` on **all** translation units → coverage
  instrumentation so the fuzzer sees into the lexer/parser/LSP libraries, plus the
  UBSan oracle. (`-fno-sanitize-recover=undefined` makes UBSan abort so libFuzzer
  flags it as a crash.)
- `-fsanitize=fuzzer` on each `fuzz_*` executable only → links the libFuzzer
  driver (the `main()` and mutation engine).
- ASan is **not** turned on automatically; it composes with the existing
  `ENABLE_ASAN` flag for a platform where ASan works (currently broken on macOS
  Tahoe — see `CLAUDE.md`). Without it, over-reads are only caught when they
  happen to fault.

**Toolchain:** libFuzzer needs a Clang that ships `libclang_rt.fuzzer`. Apple clang
does **not** (the Xcode toolchain omits it); use Homebrew/upstream LLVM on macOS,
or the LLVM `clang-cl` on Windows. See `tests/fuzz/README.md` for the exact
`cmake`/`ninja` invocation.

### Findings so far

The initial campaign found four issues in code that hand-written tests never
reached:

| Finding | Component | Oracle | Status |
|---|---|---|---|
| Infinite loop on `when self.<member>` discriminant | LSP parser | hang (regression replay of `examples/lox/value.roxy`) | fixed |
| Infinite loop on stray leading token (`}` `"` `,` `::` `0x` …) | LSP parser | hang | fixed |
| Signed-overflow UB on out-of-range integer literals | lexer | UBSan | fixed |
| ~2.9 GB allocation on ~8 KB adversarial input | LSP parser | OOM | **open** — see `TODO.md` |

The two hangs traced to recovery loops that could fail to make forward progress;
see `docs/internals/lsp-server.md` → "Forward-progress invariant". Note the split
of mechanisms: the **hangs** surfaced via the *regression replay* (a valid example
simply hung), while the *coverage-guided campaign* surfaced the **UB** and **OOM**.

## Roadmap: structure-aware fuzzing

Byte-level fuzzing hammers the lexer and error recovery well, but has a ceiling:
to reach the **semantic analyzer, IR builder, lowering, VM, or C backend**, an
input must first be a syntactically (and usually semantically) valid program, and
random mutation almost never produces one. All the Roxy-specific machinery
(lifetime checking, `when` exhaustiveness, monomorphization, register allocation,
drop-plan derivation, codegen) is effectively unreachable this way.

Structure-aware fuzzing changes the unit of currency from bytes to a **Roxy AST**,
generating inputs that are valid by construction so the budget is spent exploring
the deep passes. Mutations operate on the tree (replace a subtree with another of
the same kind, insert/delete a statement, swap operands, splice subtrees between
corpus entries); the tree is unparsed to source and fed to the normal pipeline.

### The validity spectrum

The crux for a *typed* language: how valid you generate determines which passes
you reach. Each level is harder than the last and unlocks a deeper layer.

| Validity level | Must respect | Reaches |
|---|---|---|
| Lexically valid | token rules | lexer |
| Syntactically valid | the grammar | parser, sema's *error/reject* paths |
| Name-resolved | scoping (only in-scope symbols) | symbol resolution |
| Type-correct | the type system | type checker, IR builder, lowering |
| Lifetime-correct | `uniq`/`ref`/`weak`, move-state, `when` exhaustiveness | VM, C backend, drop plans, RAII codegen |

### Keeping coverage guidance

There is no need to abandon libFuzzer's evolutionary search. Two standard ways to
plug in a structural mutator:

- **`LLVMFuzzerCustomMutator`** — libFuzzer calls your mutator for the mutation
  step; corpus entries are serialized trees you deserialize, mutate structurally,
  and re-serialize. Coverage feedback and the evolving corpus are unchanged.
- **libprotobuf-mutator (LPM)** — the pragmatic route (used by Chrome, SQLite).
  Describe the AST as a protobuf schema; LPM provides a coverage-guided mutator
  over protobuf messages for free. You write only the `proto → Roxy source`
  converter:

  ```proto
  message Expr {
    oneof e {
      int64  int_lit = 1;
      Binary binary  = 2;
      VarRef var     = 3;   // index into in-scope vars, taken mod scope size
      Call   call    = 4;
    }
  }
  message Binary { BinOp op = 1; Expr lhs = 2; Expr rhs = 3; }
  ```

  ```cpp
  DEFINE_PROTO_FUZZER(const roxy_fuzz::Program& p) {
      std::string src = proto_to_roxy(p);  // the unparser you write
      compile_and_run(src);                // reuse the existing E2E harness
  }
  ```

### Type-directed generation

Making a tree *semantically* valid means building it top-down while carrying a
context — a type-checker run in reverse:

```
gen_expr(env, wanted_type):        # produce an expression of `wanted_type` valid in `env`
    if wanted_type == i32:
        choose from:
          literal_i32()
          VarRef(v)         for v in env.vars_of_type(i32)      # scoping
          Binary(+, gen_expr(env,i32), gen_expr(env,i32))       # types
          Call(f, args…)    for f in env.funcs_returning(i32)   # args by their types
          Cast(i32, gen_expr(env, some_numeric))
    pick one (weighted, under a depth budget so it terminates)
```

The `env` mirrors what sema tracks: variables in scope and their types; declared
functions/structs/enums/traits; generic parameters; and — the Roxy-specific one —
the **move-state** of `uniq` values. This is the Csmith approach (the C generator
that found dozens of GCC/LLVM bugs) applied to Roxy's type system.

### Roxy-specific constraints

To get past sema and reach the back end, the generator must not emit a program
sema would reject:

- **Scoping** — only reference in-scope symbols (pick from an `env` list). Easy.
- **Types** — the type-directed dispatch above. Medium.
- **`uniq`/`ref`/`weak` + move-state** — the hard, high-value one. After
  consuming a `uniq` value, the generator must drop it from the usable pool or it
  emits a use-after-move and is rejected before reaching the IR builder. Modeling
  this is exactly the `LifetimeChecker` invariant, and it is where lifetime-,
  drop-, and RAII-codegen bugs hide.
- **`when` exhaustiveness / tagged unions** — generate the enum and its variants
  first, stash them in `env`, then emit `when` statements with matching cases.
- **Generics + trait bounds** — only instantiate `<T: Add>` with types that
  implement `Add`; pick call args of the instantiated types.

Build these incrementally: scoping + types first (reaches the IR builder), then
move-state (reaches codegen).

### Richer oracles

Once inputs are semantically valid, the oracle can be far stronger than
crash/hang/OOM:

- **Differential testing** — Roxy has both a VM (`compile_and_run`) and a C backend
  (`compile_and_run_cpp`). Compile the *same* generated program both ways; the
  results must match. Any divergence is a miscompilation in one backend — the class
  of bug unit tests miss most. This is the single highest-value oracle a Roxy
  structured fuzzer unlocks.
- **Valid-program-shouldn't-crash-the-compiler** — any well-typed, lifetime-correct
  program that makes sema/IR/lowering `assert` is a compiler bug by definition
  (e.g. the register-overflow item in `TODO.md`: generate a huge function and watch
  lowering fall over).
- **Round-trip stability** — with an AST→source printer, `parse(unparse(ast))`
  should be structurally identical to `ast`, and `unparse` idempotent — catching
  parser/printer disagreements.

### Staged plan

1. **Syntactic generator** — protobuf schema for the grammar + `proto_to_roxy` +
   LPM. No type tracking. Immediately fuzzes the parser and all of sema's *reject*
   logic far better than byte mutation. Oracle: crash/hang.
2. **Scoping + type-directed generation** — produces programs that pass sema and
   reach the IR builder / lowering / VM. Oracle: valid-program-shouldn't-crash **+
   the VM-vs-C differential**. Where the deep bugs start surfacing.
3. **Move-state / lifetime modeling** — reach the RAII/drop/codegen paths that are
   hardest to cover with hand-written tests.

**Caveat:** a semantically-valid generator is a *second implementation* of Roxy's
type-and-lifetime rules. Disagreements with the compiler produce false positives
(generator thinks a program is valid, compiler rejects it) and are a maintenance
cost — but the disagreements are themselves often bugs in one side or the other.

## Files

| File | Role |
|---|---|
| `tests/fuzz/fuzz_targets.hpp` | Harness body declarations + `detail::SourceBuffer` |
| `tests/fuzz/fuzz_one_{lexer,parser,lsp_parser}.cpp` | Per-component harness bodies (`rx::fuzz::fuzz_one_*`) |
| `tests/fuzz/fuzz_{lexer,parser,lsp_parser}.cpp` | `LLVMFuzzerTestOneInput` entry points |
| `tests/fuzz/corpus/` | Seed corpus + saved crash reproducers |
| `tests/fuzz/README.md` | Build/run quickstart |
| `tests/unit/test_fuzz_regression.cpp` | Always-on `Fuzz Regression` doctest replay |
| `CMakeLists.txt` | `ENABLE_FUZZERS` option + fuzz executables |

The fuzz targets link `roxy_shared` (lexer), `roxy_compiler` (parser), and
`roxy_lsp` (LSP parser); the regression replay is compiled into `roxy_tests`.

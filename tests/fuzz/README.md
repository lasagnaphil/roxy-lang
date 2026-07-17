# Fuzzing the lexer, parser & full pipeline

Coverage-guided [libFuzzer](https://llvm.org/docs/LibFuzzer.html) targets — three
byte-level harnesses for the text-facing front-end components, plus a
structure-aware harness that drives the whole compiler and VM with
valid-by-construction generated programs:

| Target            | Component                       | Harness body                    |
|-------------------|---------------------------------|---------------------------------|
| `fuzz_lexer`      | `rx::Lexer` (tokenize to EOF)   | `fuzz_one_lexer.cpp`            |
| `fuzz_parser`     | `rx::Parser` (fail-fast AST)    | `fuzz_one_parser.cpp`          |
| `fuzz_lsp_parser` | `rx::LspParser` (error-recovering CST) | `fuzz_one_lsp_parser.cpp` |
| `fuzz_structured` | full pipeline + VM (input = entropy for the `gen/` structural generator, so mutations mutate *program structure*) | `fuzz_one_structured.cpp` |

`gen/` also builds standalone (no fuzzer toolchain) as the `roxy_gen`
benchmark-corpus CLI — see `docs/internals/fuzzer.md` ("Structural generator")
and `docs/internals/profiling.md` ("Benchmark corpora at scale").

The `LLVMFuzzerTestOneInput` entry points live in `fuzz_<target>.cpp`; the actual
harness logic is the shared `rx::fuzz::fuzz_one_*` functions declared in
`fuzz_targets.hpp`. Each input is copied into an exact-size, **non-null-terminated**
heap buffer before lexing — deliberately adversarial, since production always
null-terminates its source, so any over-read is a real out-of-bounds access.

## Toolchain requirement

libFuzzer needs a Clang that ships `libclang_rt.fuzzer`. **Apple clang does not**
(the Xcode toolchain omits it). On macOS, install upstream LLVM:

```sh
brew install llvm      # provides /opt/homebrew/opt/llvm/bin/clang++
```

On Linux use distro/upstream Clang; on Windows the LLVM installer's `clang-cl`
ships the runtime.

## Build

Configure a dedicated build directory with the fuzzer-capable compiler:

```sh
cmake -B build-fuzz -G Ninja -DENABLE_FUZZERS=ON \
  -DCMAKE_C_COMPILER=$(brew --prefix llvm)/bin/clang \
  -DCMAKE_CXX_COMPILER=$(brew --prefix llvm)/bin/clang++
ninja -C build-fuzz fuzz_lexer fuzz_parser fuzz_lsp_parser fuzz_structured
```

`ENABLE_FUZZERS=ON` coverage-instruments every TU (`-fsanitize=fuzzer-no-link`)
so the fuzzer sees into the lexer/parser/LSP libraries, links the libFuzzer
driver into each `fuzz_*` executable, and enables UBSan (aborting on undefined
behavior, which libFuzzer reports as a crash). Asserts stay enabled.

ASAN is **not** turned on automatically — it composes with the existing
`-DENABLE_ASAN=ON` flag. Enable it too for full memory-error detection on a
platform where ASAN works (currently broken on macOS Tahoe — see `CLAUDE.md`);
without it, over-reads are only caught when they happen to fault.

## Run

Seed from the repo's example programs and the checked-in edge-case corpus, and
let libFuzzer grow its own working corpus. `-max_len` caps input size (which also
bounds parser recursion depth):

```sh
mkdir -p build-fuzz/corpus-parser
./build-fuzz/fuzz_parser -max_len=8192 \
  build-fuzz/corpus-parser tests/fuzz/corpus examples
```

Same shape for `fuzz_lexer` and `fuzz_lsp_parser`. Useful flags:

- `-runs=N` — stop after N inputs (CI smoke test, e.g. `-runs=1000000`).
- `-max_total_time=S` — stop after S seconds.
- `-jobs=N -workers=N` — parallel fuzzing.
- `-timeout=S` — per-input timeout; catches non-termination (hangs).
- `-rss_limit_mb=M` — abort on runaway memory.

Reproduce a saved crash:

```sh
./build-fuzz/fuzz_parser crash-<hash>
```

## When the fuzzer finds a crash

1. Minimize it: `./build-fuzz/fuzz_parser -minimize_crash=1 crash-<hash>`.
2. Fix the underlying bug.
3. Copy the (minimized) reproducer into `tests/fuzz/corpus/` with a descriptive
   name. The `Fuzz Regression` doctest suite replays everything there through all
   three harnesses on every normal `roxy_tests` run, so the crash stays fixed —
   no fuzzer toolchain required for the regression to bite.

## Regression test (no fuzzer toolchain needed)

`tests/unit/test_fuzz_regression.cpp` replays the seed corpus, every
`examples/*.roxy`, and a set of inline adversarial inputs through all three
harnesses as part of the ordinary test suite:

```sh
./build/roxy_tests --test-suite="Fuzz Regression"
```

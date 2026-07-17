# Profiling the Compiler & Interpreter

How to measure where time goes across the whole pipeline, systematically and
reproducibly. The single most important idea: **the compiler and the interpreter
are two different regimes and must be profiled separately.** A compute benchmark
like `nbody` spends 99.9% of its wall-clock in the VM — profiling it end-to-end
tells you nothing about the compiler. Pick a workload that isolates the regime
you care about, then use the matching tool.

```
Source → Lexer → Parser → Sema → IR build → IR optimize → Lower → Bytecode → VM
        └──────────────── compiler (compile time) ───────────────┘   └ interpreter (run time)
```

---

## Layer 0 — always profile an optimized build

**Never profile the default `build/`** — it is a `Debug` build (`-O0 -g`, asserts
live). With no inlining and hot `assert()`s, the function ranking is meaningless.
Use a dedicated `RelWithDebInfo` build (`-O2 -g -DNDEBUG`, frame pointers kept for
clean sampled stacks):

```bash
cmake -B build-profile -G Ninja \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
ninja -C build-profile roxy
```

`build-profile/` and `build-bcprofile/` (below) are git-ignored.

---

## Layer 1 — the built-in dashboard (no external tools)

Two always-available instruments answer "which *phase* dominates" before you ever
open a sampling profiler.

### Compiler: `roxy --time`

Every `compile()` records a per-phase wall-clock breakdown (`CompileTimings` in
`compiler.hpp`; the steady_clock overhead is a handful of calls per compile, so
it is always on). The CLI prints it:

```bash
./build-profile/roxy --time program.roxy      # one compile + the program's run time
```

```
== roxy --time: compile phases ==
  parse             0.309 ms   11.3%
  sema              0.382 ms   14.0%
  ir-build          0.670 ms   24.6%
  ir-optimize       0.479 ms   17.6%
  ir-validate       0.047 ms    1.7%
  bc-lower          0.827 ms   30.4%
  link-other        0.005 ms    0.2%
  compile           2.721 ms  100.0%
  execute        3606.988 ms
```

The `execute` row is the program's `vm_call` time — the compile-vs-run split that
tells you which regime to dig into. `link-other` is the unattributed remainder
(IR merge, native binding, registry setup).

### Compiler benchmark loop: `roxy --repeat=N`

A single compile is sub-millisecond — too short for a sampling profiler to get
signal, and dominated by process startup. `--repeat=N` compiles the program `N`
times in-process (a fresh allocator + `Compiler` each iteration), reports the
**averaged** phase table, and **skips execution**. This is both the way to get a
stable phase breakdown and the in-process loop to run under a sampling profiler:

```bash
./build-profile/roxy --repeat=200 examples/lox/main.roxy   # avg of 200 compiles
```

### Benchmark corpora at scale: `roxy_gen`

The Lox interpreter (~3.4 KLOC) is the largest hand-written Roxy program. For
compile-time work at "real-life huge codebase" scale, `roxy_gen` (built with
the normal `roxy_tests` toolchain — no fuzzer needed) emits a reproducible,
valid-by-construction multi-module project of any size. It shares the
structural generator with the fuzzer (`docs/internals/fuzzer.md` → "Structural
generator"): realistic identifier distributions, a low-index-biased import DAG
(early modules accumulate fan-in like real "core" modules), structs/enums/
generics/methods/`when`/f-strings, and a checksum-printing `main` that calls
into every module.

```bash
ninja -C build roxy_gen
./build/roxy_gen --seed=7 --modules=400 --out=/tmp/corpus_400
./build-profile/roxy --time /tmp/corpus_400/main.roxy
```

Same seed → byte-identical corpus, so numbers are comparable across compiler
changes. Generate at several sizes and check time-per-KLOC stays flat — a
super-linear drift localizes the offending phase immediately:

| `--modules` | LOC | compile (RelWithDebInfo, arm64, 2026-07-17) | ms/KLOC |
|---|---|---|---|
| 25 | 15.7k | 37 ms | 2.4 |
| 100 | 66k | 138 ms | 2.1 |
| 400 | 257k | 672 ms | 2.6 |

At 257 KLOC the phase split was ir-build 36%, bc-lower 24%, ir-optimize 20%,
parse 11%, sema 9% — consistent with the single-file findings below.

### Interpreter: the bytecode opcode profiler

A separate build flag adds per-opcode count + cycle accounting to the dispatch
loop, dumped (sorted by cycles) on VM teardown:

```bash
cmake -B build-bcprofile -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_BC_PROFILE=ON
ninja -C build-bcprofile roxy
./build-bcprofile/roxy benchmarks/nbody/nbody.roxy   # prints "=== Bytecode profile ===" to stderr
```

On Apple Silicon the cycle source is `cntvct`, which may run at a fixed nominal
rate — **trust the counts and percentages for ranking, not the absolute cycles.**

---

## Layer 2 — function/line hotspots (sampling profiler)

When Layer 1 says *which phase*, a sampling profiler says *which function/line*.
On this machine (arm64 macOS):

- **samply** (recommended DX — Firefox Profiler UI): `brew install samply`, then
  ```bash
  samply record ./build-profile/roxy --repeat=2000 examples/lox/main.roxy   # compiler
  samply record ./build-profile/roxy benchmarks/nbody/nbody.roxy            # interpreter
  ```
- **Instruments Time Profiler** (ships with Xcode, already installed as
  `xctrace`): `xctrace record --template 'Time Profiler' --launch -- ./build-profile/roxy …`,
  then open the `.trace`.
- Not `perf` (Linux only); `dtrace` works but is SIP/sudo-gated — prefer the above.

Choose the workload to match the regime:

| Regime | Workload | Why |
|--------|----------|-----|
| Compiler | `examples/lox/main.roxy` (big multi-file source) under `--repeat=N`; or the adversarial 8k-statement generated bodies | large to compile, cheap to run |
| Interpreter | `benchmarks/nbody`, `mandelbrot`, `fib`, `binary_trees` | tiny source, long run |

---

## Tracy — instrumented profiler (terminal, cross-platform)

[Tracy](https://github.com/wolfpld/tracy) is vendored as a submodule
(`third_party/tracy`, pinned to a release tag) and integrated behind
`-DENABLE_TRACY=ON`. It is the cross-platform profiler for this project — the
*client* compiles into `roxy` on Windows/macOS/Linux, and its capture tooling is
fully **terminal / headless** (the GUI `tracy-profiler` is optional, for
interactive flame-graph viewing only). Unlike a sampling profiler, it records the
exact instrumented zones (mirroring `--time`) with per-zone mean/min/max/stddev
and source lines.

> On macOS, Tracy has no automatic call-stack sampling (that is a Linux-perf /
> Windows feature) — you rely on the manual instrumentation zones, which is
> exactly the phase-aligned model we want anyway.

### Build (client)

```bash
git submodule update --init third_party/tracy      # first time only
cmake -B build-tracy -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DENABLE_TRACY=ON
ninja -C build-tracy roxy
```

`ENABLE_TRACY` is OFF by default and zero-overhead when off (the `ROXY_ZONE` /
`ROXY_FRAME_MARK` macros in `core/trace.hpp` compile to nothing). It builds with
`TRACY_ON_DEMAND`, so an instrumented binary still **runs normally** when no
profiler is attached. Instrumented zones today: the compile phases (`parse`,
`sema`, `ir-build`, `coro-lower`, `ir-optimize`, `ir-validate`, `bc-lower`) and a
coarse `vm.run`; one Tracy frame is marked per compile in the `--repeat` loop.

### Build (headless capture tools, one-time)

Both tools build self-contained from the same submodule — no system packages:

```bash
cmake -S third_party/tracy/capture   -B build-tracy-tools/capture   -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-tracy-tools/capture
cmake -S third_party/tracy/csvexport -B build-tracy-tools/csvexport -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build-tracy-tools/csvexport
```

### Capture + export (terminal)

Start `tracy-capture` first — it waits for a client — then run the instrumented
binary. The `--repeat=N` loop makes the run long enough to connect and stream
(with on-demand, capture collects from the moment it connects):

```bash
CAP=build-tracy-tools/capture/tracy-capture
CSV=build-tracy-tools/csvexport/tracy-csvexport

$CAP -o lox.tracy -f &                                   # waits for the client
./build-tracy/roxy --repeat=1500 examples/lox/main.roxy  # compiler zones stream live
wait                                                     # capture finalizes on roxy exit

$CSV lox.tracy | column -t -s,                           # per-zone CSV to stdout
```

`tracy-csvexport` columns: `name, src_file, src_line, total_ns, total_perc,
counts, mean_ns, min_ns, max_ns, std_ns` (`-e` for self-time, `-u` per-event,
`-f` name filter). For the interpreter, drop `--repeat` and run a compute
benchmark so the `vm.run` zone (and future finer VM zones) show up. Open
`lox.tracy` in the `tracy-profiler` GUI for the interactive timeline.

---

## Guardrails (repo-specific)

- **RelWithDebInfo, never Debug** (Layer 0). This is the #1 mistake.
- **Loop short work in-process** (`--repeat`) to amortize startup and get samples.
- **Separate compile from run** — the `--time` split exists so you profile the
  right thing.
- **No `perf`, no `timeout` on macOS**; ASAN is disabled on this machine (see the
  ASAN note in the build docs), so don't profile ASAN builds either.
- Opcode-profiler cycles are a **relative ranking** on arm64, not absolute ns.

---

## First-pass findings (2026-07-16, RelWithDebInfo, arm64)

A baseline snapshot from the workflow above — a starting map, not a target list.

**Compiler** (Lox interpreter, avg of 200 compiles): the back half dominates —
`bc-lower` 30% (SSA→bytecode incl. register allocation) > `ir-build` 25% >
`ir-optimize` 18% > `sema` 14% > `parse` 11%. The three IR/bytecode phases are
~73% of compile time; frontend is ~25%.

**Interpreter** (`nbody`, opcode profile): field/element access dominates —
`GET_FIELD` 37% + `INDEX_GET_LIST` 15% ≈ 52% of VM cycles, ahead of the
floating-point arithmetic opcodes. `GET_FIELD` is cheap per-op (~1.5 cyc) but
runs 208M times in the physics loop.

Next drill-downs (Layer 2, not yet done): open `bc-lower` (register allocation /
`compute_liveness`) and `ir-build` (the per-scope `robin_map` copies flagged in
prior reviews) with a sampling profiler; and confirm whether `GET_FIELD` dispatch
in the interpreter can be specialized for the common small-struct case.

---

## See also

- `optimization.md` — the SSA IR optimization passes measured as `ir-optimize`
- `bytecode.md` — opcode reference for reading the opcode profile
- `vm.md` — the interpreter dispatch loop the opcode profiler instruments

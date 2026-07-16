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

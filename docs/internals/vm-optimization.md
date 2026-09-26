# VM Interpreter Optimization

Runtime optimizations of the bytecode interpreter, independent of the SSA IR passes ([optimization.md](optimization.md)) and of the compiler's own compile-time work (`../../OPTIMIZATION.md`).

**Baseline that motivated this:** the original switch-based loop ran the quicksort benchmark (100K elements) at ~86 ms — on par with Python (~87 ms), ~16x slower than C -O2 (~5 ms). Target: ~40–55 ms. The hot path is `partition`'s inner loop: `INDEX_GET_LIST`, `LE_I`, `JMP_IF_NOT`, `ADD_I`, and frequent `swap` calls (`CALL`/`RET_VOID` around 4 list-index ops).

> Measure on an optimized build, never the default `-O0` `build/`, and prefer the
> `ENABLE_BC_PROFILE` per-opcode profiler for attribution — see
> [profiling.md](profiling.md).

## In Place

Estimated gains are the planning estimates each item was chosen on.

| Optimization | Est. gain | Notes |
|---|---|---|
| Computed-goto dispatch | 25–40% | Distinct indirect-branch site per opcode; `switch` fallback for MSVC. No `while (vm->running)` test per dispatch — HALT / top-level RET `return`. |
| CALL/RET fast path | 10–20% | Pre-allocated `CallFrame` array; flat `BCFunction**` callee table; non-param register zeroing and func_idx/arg_count checks are debug-only (they guard compiler bugs, not user errors). |
| Fused compare-and-branch | 5–15% | Integer and f64 (incl. RK); see [bytecode.md](bytecode.md). f32 not fused. |
| List-index fast path | 5–10% | Direct loads for 1- and 2-slot elements (`memcpy` only for wider); index treated as `u64` so a negative index folds into the single `idx >= length` check. |
| RK operand encoding | 5–15% | Chosen over an i8-immediate `ADDI`: the constant pool covers every constant, floats included. See [bytecode.md](bytecode.md#rk-register-or-constant-encoding). |
| String constant interning | 5–20% | `vm_load_module` pre-interns String constants so `LOAD_CONST` is a pointer load (previously a heap alloc per load — a constant f-string prefix reallocated every loop iteration). Interned constants are immortal, so `STR_RETAIN`/`STR_RELEASE` no-op on them. |
| Constant folding | 2–5% | Done in the IR builder, not at lowering ([optimization.md](optimization.md) → Phase 1). |
| `STRUCT_COPY_1`–`_4` | 1–2% | Straight-line copies with implicit slot count; general `STRUCT_COPY` still loops. |

## Rejected

- **`__attribute__((flatten))` on `interpret()`** — tested; inlining every callee raised icache pressure and was slower overall.

## Open

| Optimization | Est. gain | Sketch |
|---|---|---|
| Tail calls | 10–25% (recursive) | `TAIL_CALL` reuses the current frame when the `CALL` result feeds the next `RET` directly. Requires callee `register_count`/`local_stack_slots` ≤ current and no active cleanup records (e.g. `uniq` destructors) at the call site. |
| Profile-guided optimization | 10–20% | `-fprofile-generate` / `-fprofile-use` on the interpreter build. |
| Inline trivial natives | 5–10% (list/map-heavy) | `LIST_LEN`/`LIST_CAP`/`STR_LEN` opcodes substituted for natives marked inlineable, removing call overhead in loops like `i < xs.len()`. |
| f32 compare-and-branch fusion | 3–8% | Mirror of the f64 fused opcodes. |
| Branch hints | 2–5% | `__builtin_expect` on cold error paths (null/bounds/div-by-zero/stack-overflow checks). |
| Local-stack base caching | 1–3% | Cache `local_stack + frame->local_stack_base` as an interpreter local, refreshed on CALL/RET, for `STACK_ADDR`/`SPILL_REG`/`RELOAD_REG`. |
| `memcpy` for large `STRUCT_COPY` | 1–2% | |

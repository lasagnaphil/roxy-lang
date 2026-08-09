# VM Interpreter Optimization

Optimization roadmap for the bytecode VM interpreter. These are runtime optimizations independent of the SSA IR passes (compile-time optimizations live in `optimization.md`; compiler *compile-time* work lives in `../../OPTIMIZATION.md`). See the [summary table](#updated-summary) for per-phase status — phases 1–4, 8 and 13 are done, 6, 9 and 14 landed in a different shape than planned here, and the rest are a forward-looking plan.

**Current state:** The original switch-based dispatch loop ran the quicksort benchmark (100K elements) at ~86ms — on par with Python (~87ms) and ~16x slower than C -O2 (~5ms). Phases 1–4 close a large part of that gap.

**Benchmark profile:** The quicksort hot path is `partition`'s inner loop, run millions of times. Each iteration is list indexing (`INDEX_GET_LIST`), integer comparison (`LE_I`), conditional branch (`JMP_IF_NOT`), integer arithmetic (`ADD_I`), and frequent `swap` calls (`CALL`/`RET_VOID` wrapping 4 list-index ops). The phases below target exactly these operations.

## Phase 1: Computed Goto (Threaded Dispatch) — Done

**Gain: ~25–40%.** Replace `switch(op)` with computed goto (`&&label` label-as-value, GCC/Clang). Each handler tail-jumps directly to the next via `goto *dispatch_table[op]` instead of looping back through a single shared `switch`. The CPU then sees a distinct indirect-branch site per opcode, so the branch predictor specializes per-opcode (e.g. "`LE_I` usually follows `ADD_I`"). Well-studied 1.3–1.5x speedup.

```cpp
#define DISPATCH() do { instr = *pc++; goto *dispatch_table[instr >> 24]; } while (0)
op_ADD_I: { regs[decode_a(instr)] = regs[decode_b(instr)] + regs[decode_c(instr)]; DISPATCH(); }
```

A `#if defined(__GNUC__) || defined(__clang__)` guard keeps the switch version for MSVC. The `while (vm->running)` loop is gone — handlers `DISPATCH()` at their end, and HALT / top-level RET `return`.

**Files:** `src/roxy/vm/interpreter.cpp`.

## Phase 2: CALL/RET Fast Path — Done

**Gain: ~10–20%.** Recursive algorithms call constantly (quicksort calls `swap` ~O(N log N) times). Four reductions to per-call overhead:

- **Skip register zeroing** when the SSA IR guarantees write-before-read; zero only in debug builds (`#ifndef NDEBUG`), otherwise `memset`.
- **Pre-allocated call stack** — fixed-size `CallFrame*` array indexed by depth, replacing `Vector<CallFrame>` and its per-push capacity check.
- **Flat callee pointer table** — a `BCFunction**` built at module load, so CALL is one array index instead of `Vector` index + `UniquePtr::get()`.
- **Gate bounds checks on debug** — func_idx range, arg_count, and register-space checks guard against compiler bugs, not user errors; move them behind `assert()`.

**Files:** `src/roxy/vm/interpreter.cpp`, `include/roxy/vm/vm.hpp`, `src/roxy/vm/vm.cpp`.

## Phase 3: Fused Compare-and-Branch (Integer) — Done

**Gain: ~5–15%.** The compiler emits separate compare + branch (`LE_I r5,…; JMP_IF_NOT r5,…`); fusing halves dispatch overhead for the most common loop pattern. Adds `JMP_IF_{LT,LE,GT,GE,EQ,NE}_I b, c, offset`. Encoding is two words — `[opcode:8][_:8][src1:8][src2:8]` + `[offset:32]` — keeping the full 8-bit register range and a 32-bit offset. A post-lowering peephole pass scans for `CMP + JMP_IF/JMP_IF_NOT` pairs and fuses them, skipping the fusion when the comparison result is used elsewhere.

**Files:** `include/roxy/vm/bytecode.hpp`, `src/roxy/compiler/codegen/lowering.cpp`, `src/roxy/vm/interpreter.cpp`.

## Phase 4: List Indexing Fast Path — Done

**Gain: ~5–10%.** `INDEX_GET_LIST` / `INDEX_SET_LIST` are the most frequent non-arithmetic ops in array code. The original handler `memcpy`'d even 4-byte elements and branched on `element_is_inline` every access. Two fixes:

- **Specialize by slot count** — direct loads for `element_slot_count == 1` and `== 2`, general `memcpy` only for wider elements.
- **Unsigned bounds check** — treat the index as `u64` so a negative index folds into the same `idx >= length` comparison (one branch instead of two).

**Files:** `src/roxy/vm/interpreter.cpp`.

## Phase 5: Minor Optimizations — Partial

- **5A. Drop `vm->running` loop condition — Done.** `for (;;)` instead of `while (vm->running)`; the only writers (HALT, top-level RET) already `return`. Saves one branch per dispatch.
- **5B. `__attribute__((flatten))` on `interpret()` — rejected.** Tested; inlining all callees raised icache pressure and was slower overall.
- **5C. Profile-Guided Optimization — not yet.** Build with `-fprofile-generate`, run the benchmark, rebuild with `-fprofile-use` to let the compiler lay out branches/inlining from real profiles. Typical 10–20% on interpreter-heavy code.

## Phase 6: Immediate-Operand Arithmetic (ADDI) — Superseded by RK encoding

The goal — stop materializing `LOAD_INT tmp, 1` before `ADD_I i, i, tmp` — was met by **RK (register-or-constant) opcode variants** rather than an i8 immediate field. `ADD_I_RK dst, src1, const_idx` reads its right operand from the constant pool, so it covers *every* constant (not just −128..127) and needs no separate encoding for floats. `compute_const_use_modes` marks constants whose uses are all RK-eligible and skips both their register allocation and their `LOAD_*`. See [bytecode.md](bytecode.md) → "RK Encoding".

## Phase 7: Inline Trivial Native Calls — Not started

**Gain: ~5–10% for list/map-heavy code.** `list_len()`, `list_cap()`, `string_length()` are field loads wrapped in `CALL_NATIVE` (function-pointer lookup + indirect call + return path). Dedicated opcodes (`LIST_LEN`, `LIST_CAP`, `STR_LEN dst, reg`) compile to a single pointer deref + field load. Lowering substitutes them for known-trivial natives (marked "inlineable" in the native registry), eliminating the call overhead in loops like `i < scores.len()`.

**Files:** `include/roxy/vm/bytecode.hpp`, `src/roxy/compiler/codegen/lowering.cpp`, `src/roxy/vm/interpreter.cpp`.

## Phase 8: String Constant Interning — Done

Every `LOAD_CONST` of a String used to call `string_alloc()` (heap alloc + memcpy), so a constant f-string prefix re-allocated on every loop iteration. `vm_load_module` now pre-interns every String constant into `BCConstant::as_string.obj`, making `LOAD_CONST` a pointer load. `load_constant` keeps an un-interned fallback as defense in depth. Interned constants are immortal for the module's lifetime, so the string ref-counting ops (`STR_RETAIN`/`STR_RELEASE`) no-op on them.

**Files:** `src/roxy/vm/vm.cpp`, `src/roxy/vm/interpreter.cpp`, `src/roxy/rt/string_intern.cpp`.

## Phase 9: Float Compare-and-Branch Fusion — Done for f64, not f32

`JMP_IF_{LT,LE,GT,GE,EQ,NE}_D` and their RK variants exist (0xA6–0xAF, 0xDB–0xDC) and `fuse_compare_branch()` matches `EQ_D`–`GE_D`. The f32 half is still unimplemented — f32 comparisons emit a separate compare + branch.

**Files:** `include/roxy/vm/bytecode.hpp`, `src/roxy/compiler/codegen/lowering.cpp`, `src/roxy/vm/interpreter.cpp`.

## Phase 10: Tail Call Optimization — Not started

**Gain: ~10–25% for recursive algorithms.** A `CALL` in tail position (its result register is the immediately-following `RET`'s source, no cleanup in between) can reuse the current frame instead of pushing a new one. A `TAIL_CALL func_idx, arg_count` opcode copies args to the current window's base (`memmove` for overlap), resets the local stack to the frame base, repoints `func`/`pc`, and continues without a frame push/pop.

**Constraints:** callee `register_count <= current` and `local_stack_slots <= current`; not applicable when cleanup records (e.g. `uniq` destructors) are active at the call site.

**Files:** `include/roxy/vm/bytecode.hpp`, `src/roxy/compiler/codegen/lowering.cpp`, `src/roxy/vm/interpreter.cpp`.

## Phase 11: Branch Prediction Hints — Not started

**Gain: ~2–5%.** Error paths (null/bounds/overflow checks) are cold but weighted equally by the predictor. Wrap them in `__builtin_expect(cond, 0)` to push handling code off the hot path, improving icache locality. Apply systematically to null checks (`INDEX_*`, `DEL_OBJ`), list bounds checks, div/mod-by-zero, and register/stack/call-depth checks in CALL.

**Files:** `src/roxy/vm/interpreter.cpp`.

## Phase 12: Local Stack Base Caching — Not started

**Gain: ~1–3%.** `STACK_ADDR`, `SPILL_REG`, `RELOAD_REG` recompute `vm->local_stack.get() + frame->local_stack_base` every access. Cache it as a `u32* local_base` local alongside `regs`/`pc`, updated on CALL/RET, so those handlers just add the slot offset.

**Files:** `src/roxy/vm/interpreter.cpp`.

## Phase 13: Constant Folding — Done (in the IR, not at lowering)

Done as an SSA IR pass rather than at emission: constant folding, algebraic simplification, and cast folding are applied eagerly during IR building (`compiler/ir/ir_fold.cpp`), so lowering never sees a constant-to-constant op. See [optimization.md](optimization.md) → Phase 1.

## Phase 14: Specialized small-struct copy — Done (as opcodes, not memcpy)

`STRUCT_COPY_1`–`STRUCT_COPY_4` (0xBA–0xBD) handle the common 1–4-slot cases with a straight-line load/store sequence and an implicit slot count, removing the loop entirely. The general `STRUCT_COPY` still loops over slots; switching it to `memcpy` for large structs remains open (~1–2%).

**Files:** `src/roxy/vm/interpreter.cpp`.

## Updated Summary

| Phase | Optimization | Expected Gain | Effort | Status |
|-------|-------------|---------------|--------|--------|
| 1 | Computed goto | 25-40% | Medium | Done |
| 2 | CALL/RET fast path | 10-20% | Low-Medium | Done |
| 3 | Fused compare+branch (integer) | 5-15% | Medium | Done |
| 4 | List index fast path | 5-10% | Low | Done |
| 5 | Minor optimizations | 1-5% each | Trivial-Low | Partial (5A done, 5B rejected, 5C not yet) |
| 6 | Immediate-operand arithmetic (ADDI) | 5-15% | Medium | Superseded — RK opcode variants |
| 7 | Inline trivial natives (LIST_LEN, etc.) | 5-10% | Low-Medium | Not started |
| 8 | String constant interning | 5-20% | Low | Done |
| 9 | Float compare-and-branch fusion | 3-8% | Low | Done (f64); f32 not started |
| 10 | Tail call optimization | 10-25% | High | Not started |
| 11 | Branch prediction hints | 2-5% | Trivial | Not started |
| 12 | Local stack base caching | 1-3% | Trivial | Not started |
| 13 | Constant folding | 2-5% | Medium | Done — in the IR builder |
| 14 | Specialized small-struct copy | 1-2% | Trivial | Done (1–4 slots); general memcpy open |

**Target:** Bring quicksort from ~86ms toward ~40–55ms (2x faster than Python, ~8–10x of C).

> Measure on an optimized build, never the default `-O0` `build/`, and prefer the
> `ENABLE_BC_PROFILE` per-opcode profiler for attribution — see
> [profiling.md](profiling.md).

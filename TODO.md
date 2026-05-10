# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-05-10 (closures landed end-to-end; bounded quantification Phase B landed; C backend Phase 3 landed — runtime library, C++ RAII/container wrappers, header generation with `make_<T>` factories; C backend Phase 4 steps 1–3 landed — `roxy_ctx` thread-local context, `RoxyVM` ctx member, AOT main wrapper)


---

## High Priority

(none currently)

---

## Medium Priority

(none currently)

---

## Low Priority

(none currently)

---

## Planned Features

- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration)
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)
- [ ] AOT compilation to C — Phases 1–3 complete; Phase 4 partial (`roxy_ctx` thread-local context, `RoxyVM` ctx member, AOT wrapper `main()` that calls `main_entry()`). Phase 4 remainder: drop `RoxyVM*` from native signatures, AOT dispatch for user-registered natives via `NativeRegistry`, alias `rx::Roxy*` wrappers to `roxy::*`. Phase 5 (polish: `#line` directives, DCE, debug-name locals) still planned. See `docs/internals/c-backend.md`.

---

## Code Quality Improvements

- [ ] Standardize error message formatting across compiler
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)

---

## Bytecode VM Opcode Improvements

From a 2026-04-26 review comparing Roxy's opcode set against Lua 5.4, LuaJIT, CPython 3.13, Wren, JVM, and V8 Ignition. The base design (register-based, 32-bit fixed-width ABC/ABI/AOFF, computed-goto dispatch, type-specialized arithmetic, fused i64 compare+branch) is Lua-class and sound. Items below are concrete deltas, ordered roughly by ROI.

### High-ROI

- [x] **RK (register-or-constant) operand encoding.** *Landed 2026-04-26.* Chose opcode-variant RK (separate `*_RK` opcodes that read `c` as a constant pool index) over Lua's operand-bit RK to keep 8-bit register fields and avoid invasive spill rework. Added 22 RK opcodes covering arithmetic + f64 comparisons; deferred integer comparisons until the matching `JMP_IF_*_I_RK` fused variants land (current fusion beats RK+separate-branch). Pre-pass `compute_const_use_modes` skips `LOAD_INT`/`LOAD_CONST` when every use is RK-eligible. Specialized inline loaders (`rk_const_i64`/`f32`/`f64`) bypass the `load_constant` switch on hot paths. **Wall-time results:** Mandelbrot 357→305 ms (-14.6%), N-body 518→498 ms (-3.9%), Quicksort 52.2→48.6 ms (-6.9%). Mandelbrot's dynamic instruction count dropped 14% (302M→261M); LOAD_CONST emissions dropped 98% (28M→0.6M).
- [x] **Widen `CALL` `func_idx` from 8 to 16 bits.** *Landed 2026-04-28.* Made `CALL`/`CALL_NATIVE` two-word: `[op dst _ arg_count][func_idx:32]`. Lifted not just to 16-bit but full 32-bit; upper bits reserved for future inline-cache slots / tail-call flags. Also widened `CallData::native_index` and `IRBuilder::emit_call_native` from u8 to u32 to remove the silent truncation upstream. Wall-time impact on benchmarks: within noise (Quicksort, the only call-heavy workload at 1.25M CALLs, pays one extra `*pc++` per call ≈ +1.6%).
- [x] **Add fused float compare+branch.** *Landed 2026-04-28.* f64 only (12 new opcodes: 6 non-RK at `0xA6-0xAB`, 6 RK at `0xAC-0xAF` + `0xDB-0xDC`); f32 deferred since no benchmark uses it. The RK-fused variants (`JMP_IF_LE_D_RK` etc.) collapse mandelbrot's `GT_D_RK + JMP_IF_NOT` pattern into a single dispatch, **dropping mandelbrot wall time 45%** (340 ms → 185 ms on current machine state). Refactored `fuse_compare_branch()` with a unified `pick_fused()` helper covering all three families (int, f64-non-RK, f64-RK). Marked f64 compares as unfusable when their result outlives the block, mirroring the existing integer-compare guard; same logic added to `try_emit_rk_binary`.

### Medium-ROI

- [x] **Delete `AND` (0x61) and `OR` (0x62).** *Landed 2026-05-01.* Audit confirmed: source-level `&&`/`||` lower to short-circuit branches in `gen_binary_expr`, never to `IROp::And/Or`. The only producer of `IROp::Or` in the codebase is `gen_when_stmt` combining `EqI` results — both operands are guaranteed 0/1, so `BIT_OR` is bit-identical to `OR`. `IROp::And` has zero producers. `get_opcode` now maps `IROp::And/Or` → `BIT_AND/BIT_OR`. Opcode slots 0x61-0x62 freed; handlers and dispatch entries removed. Tests pass; no benchmark regression.
- [x] **Specialized small-struct copy.** *Landed 2026-05-01.* Audit corrected the original note: `STRUCT_COPY` is one-word ABC (not two-word), with a runtime `for (i < slot_count) dst[i] = src[i]` loop. Existing benchmarks (mandelbrot, nbody, quicksort) hit it ≤35 times total — not a hot path. Added a struct-copy-heavy microbenchmark (`benchmarks/struct_copy/`) where `STRUCT_COPY` was 14.5% of cycles; specialization brings the 2-slot case from 0.042 → 0.029 cyc/op (-31% per-op) by replacing the loop with straight-line stores. Added `STRUCT_COPY_1/2/3/4` covering Vec2 / Vec3 / Color and similar shapes; lowering picks the specialized opcode in `IROp::StructCopy` when slot_count ∈ [1,4]. Function-prologue `STRUCT_COPY` is gated on `slot_count > 4` so doesn't need specialization. No measurable change on existing benchmarks (struct copies aren't on their hot paths).
- [ ] **Inline-cache slot in `CALL_METHOD` for trait/vtable dispatch.** Not needed today (no virtual dispatch yet), but cheap to design in now and painful to retrofit. Reserve 1–2 words per call site for resolved function pointer + monomorphic guard. Reference: V8 Ignition feedback vectors, Smalltalk PIC.

### Low-ROI / Speculative

- [ ] **Wider immediate for `LOAD_INT`.** Currently signed 16-bit; constants outside ±32K hit the constant pool. Lua 5.4 added `LOADI` with 24-bit signed sBx. Worth it only if profiler shows meaningful `LOAD_CONST` traffic for small-but-out-of-range integers.
- [ ] **Verify i32 `DIV_I`/`MOD_I` edge cases.** All integer ops route through i64 (`interpreter.cpp:688-705`). Add/sub/mul are bit-identical between i32 and i64 wrapping, but `INT32_MIN / -1` differs (i64 path silently produces a different result; i32 hardware would trap). Confirm SSA lowering inserts a `TRUNC_S` or that this case is unreachable.

---

## Testing Gaps

- [ ] Test deeply nested struct field access (>5 levels; currently only 3 levels tested)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs (diamond dependencies, >3 levels)
- [ ] Test variable shadowing with noncopyable types (inner scope same name as moved outer variable)
- [ ] Test struct literals with noncopyable field values (source variable should be marked moved)
- [ ] Test self-assignment of noncopyable variables (`x = x`)

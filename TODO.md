# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-07-05 (semantic analyzer deep review added — 8 probe-verified bugs + refactoring plan, see "Semantic Analyzer Refactoring & Bugs" section)


---

## High Priority

- [ ] Semantic analyzer verified bugs — 8 probe-confirmed issues (silent error-type leak for unsupported primitive operators, flat enum-variant namespace, wrong-enum `when` cases, generics×lambdas broken, etc.). See "Semantic Analyzer Refactoring & Bugs" below.

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
- [ ] AOT compilation to C — Phases 1–4 fully complete (codegen, runtime library + C++ wrappers, header generation, runtime unification across VM/AOT, `RoxyVM*` dropped from native signatures, AOT NativeRegistry dispatch with `aot_symbol_name` override + dual-mode `bind_native` + auto-emitted extern decls, `MapHeader` slimmed via per-VM dispatch side-table). Phase 5: function-level + statement-level `#line` directives in generated AOT source landed (debuggers attribute every IR-emitted statement to its Roxy source line). Other Phase 5 items (DCE, Relooper, `switch` lowering, readable variable names) deliberately not pursued — the C compiler's optimizer handles them and they don't affect debugger UX. See `docs/internals/c-backend.md`.

---

## Code Quality Improvements

- [ ] Standardize error message formatting across compiler
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)

---

## Semantic Analyzer Refactoring & Bugs

From a 2026-07-05 deep review of `semantic.cpp` (6.6k lines) and collaborators (`symbol_table.cpp`, `types.cpp`, `compiler.cpp`, `generics.cpp`). Every bug below was **verified by running probe programs through the `roxy` CLI**; none is documented as an intended limitation. Suggested sequence: pin bugs with regression tests → mechanical dedup → semantic fixes → subsystem extraction → context/naming polish.

### Verified bugs (probe-confirmed)

- [ ] **Unsupported primitive operators silently produce `Error` type — no diagnostic, garbage output.** `register_primitive_operator_methods` (semantic.cpp:2954) only registers arithmetic for I32/I64/F32/F64 (plus `eq`/`ne` on bool/string). For `u32 + u32` the fallback in `get_binary_result_type` (semantic.cpp:6501) calls `require_types_match` — which passes silently when the types match — then returns `error_type` unconditionally. Probe: `var x: u32 = 40; var y: u32 = 2; var z = x + y; print(f"{z}");` compiles with zero errors and prints an empty line (f-string interp skips error-typed values); the annotated variant `var z: u32 = x + y` prints 42 only because the IR builder types the add off the operands. Affects u8/u16/u32/u64/i8/i16 arithmetic, comparisons, unary ops, compound assigns. Fix: data-driven operator registration over all numeric TypeKinds + make the fallback error loudly ("operator not defined for type") as a backstop.
- [x] **printf-style format strings fed to the `{}`-only formatter.** *Landed 2026-07-05.* Converted both sites to `{}` placeholders. Also fixed a related truncation family: `TypeChecker::type_string` counts its trailing NUL in `String::size()`, so passing the `String` directly to `error_fmt` embeds a NUL mid-message (message cut at the type name) — three direct-pass sites now use `.data()` like the rest. Follow-up idea kept open: compile-time format check.
- [x] **Tagged-union `when` clauses accept variants of the wrong enum.** *Landed 2026-07-05.* `resolve_when_clauses` now requires `sym->type == discriminant_type` ("'Small' is not a variant of enum 'Color'"), and the VariantInfo pass only reads discriminant values off validated symbols. (The per-enum variant-table item below will replace the global lookup entirely.)
- [x] **Enum variants live in a flat global namespace.** *Landed 2026-07-05.* `EnumTypeInfo` now carries a per-enum variant table (`Span<EnumVariantInfo>` + `find_variant`), populated by `resolve_enum_members`. `Enum::Variant` (semantic + IR builder), when-statement cases (semantic + IR builder — the IR builder had the same collision bug and would have emitted the wrong constant), and tagged-union when clauses all resolve through it; enum imports now read the table instead of recomputing values from the decl (dedup win); `analyze_when_stmt` no longer dereferences a possibly-null decl. Bare unqualified variant names still use the flat scope (inherent to bare names).
- [x] **Value-struct forward references misdiagnosed as infinite recursion.** *Landed 2026-07-05.* Struct member resolution is now memoized and recursive (`ensure_struct_members_resolved` + `StructTypeInfo::members_resolved`): value-embedded field structs, parents, tagged-union variant fields, and generic-instance fields all resolve on demand, so declaration order no longer matters. Genuine value cycles (incl. mutual and through generic instances) are caught by the in-progress set with the same "infinite size" error; the post-hoc `detect_mutual_struct_recursion` recompute pass is deleted. Bonus: member decls (ctors etc.) appearing before their struct decl no longer risk being wiped by the layout pass.
- [ ] **Generic functions containing lambdas fail with "unknown type 'T'".** Verified same-module and cross-module. The generics cloner (`generics.cpp`) has no `ExprLambda` case, so lambda param/return TypeExprs are never substituted at instantiation. Latent second layer once cloning is fixed: the cross-module drain loop (compiler.cpp:277) never persists `analyzer.synthetic_decls()`, so cross-module instances containing lambdas would silently lose their lifted call functions before IR build.
- [x] **Methods can't be coroutines, with a misleading error.** *Landed 2026-07-05 (diagnostic).* `analyze_member_body` now reports "coroutine methods are not yet supported; use a free function returning Coro<T>..." and analyzes the body in coroutine context so the yields don't add the misleading placement error. Actual coroutine-method support (per-function coro type for methods + self-carrying state machine) remains future work.
- [x] **Generic type-arg inference only understands `List` among builtin containers.** *Landed 2026-07-05.* `unify_type_expr` now unifies `Map<K, V>` (key+value) and `Coro<T>` (yield type). Map inference verified end-to-end (`map_len(inout m)`). Note for Coro: inference now binds T, but *passing* a coroutine still fails assignability — annotated `Coro<T>` resolves to the interned generic type while a coroutine value carries its per-function type (`coroutine_type_for_func`); first-class coroutine values are a separate feature.

### Architectural refactorings

- [ ] **Split the god class along existing seams.** `SemanticAnalyzer` owns ~8 separable concerns in one 6.6k-line file. `ErrorReporter` was already extracted with the right pattern (shared by reference so collaborators report errors without a back-reference); extend it. Candidates in order of self-containedness: **MoveChecker** (~800 lines: move states, snapshots/merges, `m_branch_terminates`, loop cross-iteration checks, `consume_noncopyable`, scope-exit dtor checks), **LambdaCaptureAnalyzer** (~700 lines: contexts, `try_capture_identifier`, validate/synthesize/backfill, `ensure_self_captured_through`), **TraitSystem** (~700 lines: builtin registration, trait method decls, impl grouping/validation, default injection, `concretize_trait_type`), **GenericCallResolver** (~700 lines: unify/infer, both generic-call paths, template-ref coercion, bounds). Share a small `SemaContext` {allocator, type_env, types, symbols, reporter, checker, modules}.
- [ ] **Bundle per-function context state.** `m_in_coroutine`, `m_coro_yield_type`, `m_in_delete_destructor`, `m_in_finally_depth`, `m_branch_terminates`, `m_move_states`, `m_active_type_params/bounds` are guarded ad hoc with `ScopedValue` at four entry points (`analyze_fun_decl`, `analyze_member_body`, `synthesize_lambda_call_fn`, `analyze_generic_template_body`); the coroutine-method bug above is a forgotten setup at one of them. A single `FunctionContext` pushed/popped as a unit eliminates the error class.
- [ ] **Fix the naming inversion.** `analyze_constructor_decl`/`analyze_destructor_decl`/`analyze_method_decl` register signatures (Pass 2) while `analyze_fun_decl` analyzes a body (Pass 3); `resolve_*` overlaps both. Rename to `register_*_signature` vs `analyze_*_body` uniformly.
- [ ] **Make the semantic→IR annotation contract explicit.** `ce.callee->resolved_type` variously means callee function type / struct type (ctor call) / `ref(parent)` (super call) / cast target; `get_expr.object->resolved_type = nullptr` signals module access; bare generic template refs return `error_type` + `is_generic_template_ref` flag that every coercion site must remember to check. Document the contract in one place or add explicit annotation fields on the AST nodes.
- [ ] **Decide on AST mutation vs. side-band annotations.** Analysis rewrites the tree in place (captured identifiers → `ExprGet(__env, …)`, generic TypeExprs → mangled name with `type_args` cleared, struct-literal names mangled), so re-analysis of the same AST is non-idempotent — a risk for LSP Phase 8 and incremental compilation.
- [ ] **Normalize the `resolve_type_expr` null contract.** Returns null only for null input, yet ~30 call sites do `if (!ptype) ptype = error_type()`. Make it never-null and delete the noise.

### Duplication cleanups (mechanical)

- [ ] Synthetic-dtor "needs_cleanup" scan (fields + variant fields) ×3: semantic.cpp:834, 1087, 1265 → `struct_needs_synthetic_dtor()` + `add_synthetic_default_dtor()` helpers.
- [ ] Pending-generic-fun drain (ownership test + sideline) ×2: semantic.cpp:290 vs 1124.
- [ ] Enum-variant value computation ×2: semantic.cpp:587 (resolve) vs 2054 (import) — both silently ignore non-literal constant expressions (`enum E { A = 1 + 2 }` auto-increments instead); dedup and either support const-exprs or error.
- [ ] Super-call argument checking ×3 inside `analyze_super_call` (semantic.cpp:5346).
- [ ] Crossed-lambda-boundary scope walk ×3: semantic.cpp:3886, 4217, 6072.
- [ ] Ctor/dtor registration near-duplicates: `analyze_constructor_decl` vs `analyze_destructor_decl`, plus both re-implemented inline in `resolve_generic_struct_fields` (semantic.cpp:1209).
- [ ] `resolve_global_var` vs `analyze_var_decl` drift (semantic.cpp:651 vs 1478): global path lacks the nil-inference error, redefinition check, and noncopyable move tracking. Unify.
- [ ] `append_method`/`append_constructor`/`append_destructor` (semantic.cpp:2305): three copies of an O(n²) span-rebuild that also churns the arena; template it or use `Vector` during analysis and freeze to spans.
- [ ] List/Map `.copy()` element checks live inline in `analyze_call_expr` (semantic.cpp:5598); move into the builtin-method-call path.

### Performance

- [ ] **`SymbolTable::pop_scope` rebuilds the entire lookup cache** (symbol_table.cpp:50-75), walking every scope including global (all functions + builtin prelude + every enum variant) on **every block exit**. O(total program symbols) per `}`. Fix: per-name shadow stacks or save-the-shadowed-symbol-on-define.
- [ ] `lookup_local` linear-scans the current scope; prelude import calls it per export → quadratic startup on the global scope.
- [ ] Move-state snapshots copy the whole map at every branch point (if/while/for/when/try/ternary) — fine at current scale; revisit with an undo log only if profiling warrants.

### Smaller correctness-adjacent debts

- [ ] All-paths-return checking is a stub (semantic.cpp:1971) while `m_branch_terminates` already computes definite termination and `Scope::function.has_return`/`mark_return` machinery exists but is never read — finish missing-return diagnostics nearly for free, or delete the dead machinery.
- [ ] `is_hashable_key_type` accepts every struct (bytewise hashing) while the error text says "must implement Hash" — align message and semantics.
- [ ] Stale comment in `analyze_delete_stmt` (semantic.cpp:3373): says struct-field deletes are forbidden, but `check_not_field_move` permits pointer-field deletes by design.
- [ ] Audit `try`/`finally` move-state entry: the finally body is analyzed against the merged *normal* exits only; the exceptional pass-through path isn't merged in. Traced as conservative in cases tried, but deserves a test.
- [ ] LSP-mode null tolerance is ad hoc: some paths null-check everything, others dereference decl/info chains unconditionally (e.g., `eti.decl->enum_decl` at semantic.cpp:3446). Define a per-pass policy.
- [ ] Manual string building with `push_back` loops in `check_type_arg_bounds` (semantic.cpp:2512) next to a perfectly good `format_to_arena`.

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

- [ ] Regression tests for the 8 verified semantic-analyzer bugs (see "Semantic Analyzer Refactoring & Bugs"); each item there includes a minimal repro sketch
- [ ] Test deeply nested struct field access (>5 levels; currently only 3 levels tested)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs (diamond dependencies, >3 levels)
- [ ] Test variable shadowing with noncopyable types (inner scope same name as moved outer variable)
- [ ] Test struct literals with noncopyable field values (source variable should be marked moved)
- [ ] Test self-assignment of noncopyable variables (`x = x`)

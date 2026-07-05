# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-07-05 (semantic-analyzer refactoring sweep: the god-class split is **finished** — LifetimeChecker, TraitSystem, GenericCallResolver extracted behind a shared SemaContext; lambda capture analysis deliberately stays inline. FunctionContext bundling landed (all per-function slots push/pop as one unit). The naming inversion is fixed (`register_*_signature` vs `analyze_*_body`). Each step surfaced and fixed a leak of the same "forgot one slot / wrong dispatch" class: lambda branch-terminates, lambda in-delete-destructor, and the LSP member-body dispatch. The 8 verified bugs from the earlier deep review are fixed in commits `b7e9ab0`..`0fb1b44`.)

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

- [ ] **Unsigned & small-int arithmetic** (u8/u16/u32/u64/i8/i16 beyond `==`/`!=`): needs unsigned IR ops (compare/div/mod/shr), width-wrapping semantics for narrow arithmetic (post-op narrowing or dedicated opcodes), VM opcode support, and a C-backend signedness audit (it emits from the same signed-i64 IR). The semantic layer rejects these with "operator not supported" instead of silently mistyping (since 2026-07-05).
- [ ] **First-class coroutine values**: passing or returning a `Coro<T>` fails assignability — an annotated `Coro<T>` resolves to the interned generic type while a coroutine value carries its per-function type (`coroutine_type_for_func`, which encodes the mangled `resume`/`done` names). Needs unified typing plus dynamic resume/done dispatch. Inference already binds `T` from a coro argument.
- [ ] **Coroutine methods** (`fun S.count(): Coro<i32>`): rejected with an explicit "coroutine methods are not yet supported" error (since 2026-07-05); needs the per-function coro type for methods plus a `self`-carrying state machine in coroutine lowering.
- [ ] **Casting unsuffixed integer literals to small int types** fails: `u8(200)` → "cannot cast 'i32' to 'u8'" because the literal has IntLiteral kind, which `TypeChecker::can_cast` doesn't treat as numeric. Casting a *variable* works. Coerce the literal (or accept IntLiteral) in `analyze_primitive_cast`.
- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] LSP server Phase 8: full semantic analysis (TypeCache/TypeEnv integration)
- [ ] LSP server Phase 9: polish (signature help, code actions, workspace symbols, semantic tokens)
- [x] AOT compilation to C — **feature-complete**: every language feature compiles through the C backend, `roxy_ctx` runtime unification across VM/AOT, header generation, `#line` directives for debugger attribution. Remaining codegen-quality items (DCE, Relooper, `switch` lowering, readable variable names) deliberately not pursued — the C compiler's optimizer covers them. See `docs/internals/c-backend.md`.

---

## Code Quality Improvements

- [ ] Standardize error message formatting across the compiler; consider a compile-time `{}`-placeholder check for `error_fmt`/`format_to` (the printf-residue bugs fixed 2026-07-05 would have been caught at build time)
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)

---

## Semantic Analyzer Refactoring

From a 2026-07-05 deep review of `semantic.cpp` (6.6k lines) and collaborators (`symbol_table.cpp`, `types.cpp`, `compiler.cpp`, `generics.cpp`). The review also surfaced 8 probe-verified bugs — all fixed the same day (commits `b7e9ab0`..`0fb1b44`: error-message rendering, wrong-enum `when` cases, Map/Coro inference, coroutine-method diagnostic, per-enum variant tables, declaration-order-independent struct resolution, loud operator errors, generics×lambdas). What remains is the refactoring backlog, roughly ordered mechanical → structural. References use function names rather than line numbers (which rot).

### Architectural refactorings

- [x] ~~**Split the god class along existing seams** — first extraction~~: **LifetimeChecker** landed (renamed from the review's "MoveChecker" — it owns termination and scope-exit destructor checks too, not just moves). `compiler/lifetime_checker.{hpp,cpp}`: move states, snapshots/merges, the branch-terminates flag, loop cross-iteration checks, `consume_noncopyable`, scope-exit dtor checks — shared by reference (SymbolTable/TypeCache/ErrorReporter), no back-reference to the analyzer. The extraction also fixed a probe-verified soundness bug: `synthesize_lambda_call_fn` guarded move states but not `m_branch_terminates`, so a `return` inside a lambda body leaked "this branch terminates" into the enclosing `if` merge, which then discarded the branch's move states (use-after-move false negative → double-free). `LifetimeChecker::FunctionScope` now saves/clears/restores both as a unit at all four body-analysis entry points; regression test in `test_raii.cpp` ("conditional move is still checked when a lambda body ends in return").
- [x] ~~**Split the god class — TraitSystem + SemaContext**~~: **TraitSystem** landed (`compiler/trait_system.{hpp,cpp}`, ~720 lines): builtin trait registration (incl. `register_primitive_operator_methods` and the Pass 1.7 Printable/Hash/Eq/Exception/Index setup), user trait-decl collection, trait method decls, impl grouping/validation, default injection, `concretize_trait_type`. The **`SemaContext`** from this list also landed (`compiler/sema_context.hpp`): the {allocator, type_env, types, modules, symbols, reporter, checker} bundle passed to collaborators (LifetimeChecker migrated to it), plus a function-pointer thunk exposing the analyzer's `resolve_type_expr` — the one analyzer operation trait code needs — without a back-reference (and without introducing virtual dispatch, which this codebase avoids). `append_method`/`append_constructor`/`append_destructor` moved to free functions in `types.{hpp,cpp}` (shared by analyzer + trait system; their O(n²) span-rebuild is still the open item below). Trait *bounds* checking and Phase B type-param method lookup deliberately stayed on the analyzer — they belong to GenericCallResolver.
- [x] ~~**Split the god class — GenericCallResolver**~~: landed (`compiler/generic_call_resolver.{hpp,cpp}`, ~735 lines): type-arg unification/inference, both generic-fun-call paths, template refs in value position (`coerce_generic_template_ref` / explicit `identity<i32>`), trait-bound resolution/checking (Phase A), and Phase B definition-site template-body checking — including the active-type-param state, which the analyzer's `resolve_type_expr` and operator/method dispatch now consult through accessors (`has_active_bounds` / `resolve_active_type_param` / `lookup_type_param_method` / `substitute_trait_types`). `SemaContext` gained `analyze_expr` / `analyze_stmt` thunks (generic inference analyzes call arguments; Phase B walks template bodies). `ScopedValue` promoted from semantic.cpp's anonymous namespace to `core/scoped_value.hpp` (shared with the resolver's Phase B guards).
- [x] ~~**Split the god class — LambdaCaptureAnalyzer**~~: **deliberately not pursued** — the god-class split is considered finished with the three landed extractions. Lambda capture analysis is too entangled with the analyzer to be worth a class boundary: `try_capture_identifier` and the self-capture path are interception points *inside* `analyze_identifier_expr`/`analyze_this_expr` (the walker consults the capture-context stack mid-expression), `synthesize_lambda_call_fn` re-enters full body analysis under the analyzer's own coroutine/lifetime guards, and the capture rewrites mutate the very AST the surrounding analysis is walking. A split would need thunks in *both* directions (walker → capture hooks and capture code → walker) — two classes pretending not to be one. Revisit only if LSP Phase 8 forces re-analyzable ASTs (see "Decide on AST mutation vs. side-band annotations" below).
- [x] ~~**Bundle per-function context state**~~: landed. `FunctionContext` (`compiler/function_context.hpp`) holds the coroutine / delete-destructor / finally-depth slots; `FunctionContextScope` saves it, installs a fresh default, and bundles `LifetimeChecker`'s per-function state into the same push/pop — every body-analysis entry point (`analyze_fun_decl`, `analyze_member_body` — which now takes `is_delete_destructor` instead of a caller-side guard in `analyze_destructor_body` —, `synthesize_lambda_call_fn`, and the resolver's `analyze_generic_template_body`) holds exactly ONE guard object, so there is no slot to forget. Bundling exposed and fixed a third leak of the same class: the in-delete-destructor flag leaked into synthesized lambda bodies, spuriously rejecting a `throw` inside a lambda *defined in* a delete destructor (inconsistent — calling a named throwing function from the destructor is allowed; the lambda's statements run when the closure is called, not during destruction). Regression subcase in `test_exceptions.cpp` ("Throw inside a lambda defined in a delete destructor is allowed").
- [x] ~~**Fix the naming inversion**~~: landed. Pass 2 signature registration is now `register_constructor_signature`/`register_destructor_signature`/`register_method_signature`/`register_fun_signature` (+ `TraitSystem::register_trait_method_signature`); Pass 3 body analysis is `analyze_fun_body` alongside the existing `analyze_*_body` family. Dead code deleted along the way (`analyze_decl`, `analyze_struct_decl`, `analyze_enum_decl` — zero callers). The rename also exposed a real casualty of the inversion: `analyze_single_function` — the LSP per-function body-analysis entry — dispatched methods/ctors/dtors to the signature-registration methods, so LSP body analysis of a member re-reported a spurious "duplicate method/constructor" diagnostic and never analyzed the body. It now dispatches to the `analyze_*_body` family (looking up the receiver struct type); regression tests in `test_lsp_analysis_context.cpp` ("analyze method body" ×2).
- [ ] **Make the semantic→IR annotation contract explicit.** `ce.callee->resolved_type` variously means callee function type / struct type (ctor call) / `ref(parent)` (super call) / cast target; `get_expr.object->resolved_type = nullptr` signals module access; bare generic template refs return `error_type` + `is_generic_template_ref` flag that every coercion site must remember to check. Document the contract in one place or add explicit annotation fields on the AST nodes.
- [ ] **Decide on AST mutation vs. side-band annotations.** Analysis rewrites the tree in place (captured identifiers → `ExprGet(__env, …)`, generic TypeExprs → mangled name with `type_args` cleared, struct-literal names mangled), so re-analysis of the same AST is non-idempotent — a risk for LSP Phase 8 and incremental compilation.
- [ ] **Normalize the `resolve_type_expr` null contract.** Returns null only for null input, yet ~30 call sites do `if (!ptype) ptype = error_type()`. Make it never-null and delete the noise.

### Duplication cleanups (mechanical)

- [ ] Synthetic-dtor "needs_cleanup" scan (fields + variant fields) ×3: `generate_synthetic_destructors`, the generic-instance worklist in `analyze_function_bodies`, and `resolve_generic_struct_fields` → extract `struct_needs_synthetic_dtor()` + `add_synthetic_default_dtor()`.
- [ ] Pending-generic-fun drain (ownership test + sideline) ×2: `analyze_owned_pending_fun_instances` vs the worklist in `analyze_function_bodies`.
- [ ] Super-call argument checking ×3 inside `analyze_super_call` (default ctor / named ctor / method arms).
- [ ] Crossed-lambda-boundary scope walk ×3: `try_capture_identifier`, the `[move]` path of `validate_lambda_captures`, `analyze_this_expr`.
- [ ] Ctor/dtor registration near-duplicates: `analyze_constructor_decl` vs `analyze_destructor_decl`, plus both re-implemented inline in `resolve_generic_struct_fields`.
- [ ] `resolve_global_var` vs `analyze_var_decl` drift: the global path lacks the nil-inference error, redefinition check, and noncopyable move tracking. Unify.
- [ ] `append_method`/`append_constructor`/`append_destructor`: three copies of an O(n²) span-rebuild that also churns the arena; template it or use `Vector` during analysis and freeze to spans. (Now free functions in `types.{hpp,cpp}`, shared by SemanticAnalyzer and TraitSystem — the rebuild itself is still the open item.)
- [ ] List/Map `.copy()` element checks live inline in `analyze_call_expr`; move into the builtin-method-call path.

### Performance

- [ ] **`SymbolTable::pop_scope` rebuilds the entire lookup cache** (`rebuild_lookup_cache` walks every scope including global — all functions + builtin prelude + every enum variant) on **every block exit**: O(total program symbols) per `}`. Fix: per-name shadow stacks or save-the-shadowed-symbol-on-define.
- [ ] `SymbolTable::lookup_local` linear-scans the current scope; prelude import calls it per export → quadratic startup on the global scope.
- [ ] Move-state snapshots copy the whole map at every branch point (if/while/for/when/try/ternary) — fine at current scale; revisit with an undo log only if profiling warrants.

### Smaller correctness-adjacent debts

- [ ] All-paths-return checking is a stub (end of `analyze_fun_decl`) while `m_branch_terminates` already computes definite termination and the `Scope::function.has_return`/`mark_return` machinery exists but is never read — finish missing-return diagnostics nearly for free, or delete the dead machinery.
- [ ] `is_hashable_key_type` accepts every struct (bytewise hashing) while the error text says "must implement Hash" — align message and semantics.
- [ ] Stale comment in `analyze_delete_stmt`: says struct-field deletes are forbidden, but `check_not_field_move` permits pointer-field deletes by design.
- [ ] Audit `try`/`finally` move-state entry: the finally body is analyzed against the merged *normal* exits only; the exceptional pass-through path isn't merged in. Traced as conservative in cases tried, but deserves a test.
- [ ] LSP-mode null tolerance is ad hoc: some paths null-check every child, others assume decl/info chains are present (e.g. `analyze_constructor_call` dereferences `ctor->decl` — true for user ctors today, but nothing enforces it). Define a per-pass policy.
- [ ] `resolve_enum_members` only honors compile-time integer *literals* for variant values — `enum E { A = 1 + 2 }` silently auto-increments instead. Support const exprs or reject with an error.
- [ ] Manual string building with `push_back` loops in `check_type_arg_bounds` next to a perfectly good `format_to_arena`.

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

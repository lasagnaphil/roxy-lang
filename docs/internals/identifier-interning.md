# Identifier Interning (Sym IDs) — post-mortem

> **⛔ ABANDONED — measured net regression. Do not re-attempt as specified.**
> Nothing described here as "interning" exists in the codebase. The one piece
> that was kept — the canonical mangler — is described in
> [Name mangling is canonical](#name-mangling-is-canonical-the-piece-that-was-kept)
> below.

The plan (OPTIMIZATION.md §5.1) was to intern every source identifier to a dense
`u32` symbol ID (`Sym`) at lex time, so name-keyed maps would hash and compare
integers instead of re-running an FNV-1a byte loop and a `memcmp` on every probe.
It was implemented through Phases 0–1d plus §5.2a, measured, and **reverted**.
The full design document and migration map are in this file's git history
(`docs/internals/identifier-interning.md` before 2026-08); this page keeps only
the result and the facts that outlived it.

## What was measured

On the 400-module / 257 KLOC generated corpus (`roxy_gen --seed=7 --modules=400`,
interleaved before/after floors — Lox at ~2 ms is below the noise floor and must
not be used for this class of measurement):

- **Full interning was +5.6 % total compile time** vs. the pre-`Sym` baseline —
  parse **+26.7 %**, sema **+4.3 %**.
- The driver is the per-identifier-token **hash + robin_map probe at lex**, paid
  on hundreds of thousands of tokens — far more than the downstream lookups it
  saves.
- The cost is **structural, not an implementation detail**: a no-copy
  `intern_stable` (storing the source-buffer view instead of copying bytes)
  recovered only ~6 % of the parse tax, proving the cost is the hash/probe, not
  the copy.

## Why the ceiling looked high, and why that reasoning failed

The apparent upside was real: every name is a raw `StringView` into the source
buffer, `std::hash<StringView>` is a byte-wise FNV-1a loop re-run on every map
probe in sema/ir-build/bc-lower, and field/method/variant resolution is a linear
scan doing a `memcmp` per element. Interning collapses all of that to `u32`
identity.

The error was in where the payoff was banked. It was to come from the §5.2b
`IRInst` union shrink — better locality on the IR walk, which is ~68 % of
compile time. But **§5.2a (a contiguous `IRInst` pool) measured neutral**: the IR
walk is *not* `IRInst`-cache-locality-bound (the bottleneck is the
`Vector<IRInst*>` indirection and per-op compute), so a smaller `IRInst` would
not have helped either. `BumpAllocator` cost is size-independent, so there was no
allocation-time saving either. Removing a cost downstream never paid for the cost
added at the lexer.

**Transferable lesson:** front-loading work onto the highest-frequency event in
the pipeline (one lex token) to save work on lower-frequency events (map probes)
loses unless the frequency ratio is checked first. See OPTIMIZATION.md §7 for the
negative-results register and §8 for where compile-time effort should go instead.

## Name mangling is canonical (the piece that was kept)

Interning was only *sound* if the `$$` mangling scheme had one byte-producing
definition — it was previously re-`format()`'d at eight sites across five files,
each with its own literal that had to byte-match the others. That unification
landed as a standalone refactor (`compiler/support/mangling.{hpp,cpp}`) and was kept: it
is perf-neutral and removes a real drift hazard independent of interning.

All mangled names now come from one module — `mangle_method`,
`mangle_constructor`, `mangle_destructor`, `mangle_module_local`,
`mangle_type_name`, `mangle_overload` — routed through by `semantic.cpp`,
`ir_builder.cpp`, `lowering.cpp`, `generics.cpp`, `trait_system.cpp`,
`coroutine_lowering.cpp`, and `c_emitter.cpp`.

**The `$$` spelling is a load-bearing ABI.** Separately from re-derivation, the C
emitter *parses* the byte structure of mangled names to route container methods
to runtime functions: `suffix_after_last_dollar_dollar` splits on the last `$$`,
and `ends_with(fn, "$$get" / "$$get_or" / "$$index" / "$$pop" / "$$delete")` and
the `"$$resume"` suffix pattern-match the spelling. Changing the scheme means
changing those readers.

## Structural facts worth keeping

The compiler's names come from two populations, each with one choke point — the
useful framing for any future name-representation work:

- **Population A — source identifiers** (variables, types, fields, methods,
  params). All enter through `Token::text()` and are copied into the AST by the
  parser. One choke point.
- **Population B — synthetic names** minted after lex and never present in source
  (`Box$i32`, `Vec2$$length`, `__lambda_3_env`, `__tmp7`). These funnel through
  the `IRBuilder` minting primitives (`intern_format` / `intern_synthetic_name` /
  `intern_concat`), `GenericInstantiator::mangle_name`, and the canonical
  manglers above.

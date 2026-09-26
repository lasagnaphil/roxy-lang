# Identifier Interning (Sym IDs) — post-mortem

> **⛔ ABANDONED — measured net regression. Do not re-attempt as specified.**
> Nothing described here as "interning" exists in the codebase. The one piece
> that was kept — the canonical mangler — is described in
> [Name mangling is canonical](#name-mangling-is-canonical-the-piece-that-was-kept)
> below.

The plan (OPTIMIZATION.md §5.1) was to intern every source identifier to a dense
`u32` symbol ID (`Sym`) at lex time, so name-keyed maps would hash and compare
integers instead of re-running an FNV-1a byte loop and a `memcmp` on every probe.
It was implemented, measured, and **reverted**; the full design and migration map
are in this file's git history.

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

The apparent upside was real (names are raw `StringView`s re-hashed on every map
probe; field/method/variant resolution is a `memcmp` scan). But the big payoff was
banked on a follow-up `IRInst` shrink for IR-walk locality, and **a contiguous
`IRInst` pool (§5.2a) measured neutral**: the IR walk is *not*
`IRInst`-cache-locality-bound (the bottleneck is the `Vector<IRInst*>` indirection
and per-op compute). Removing a cost downstream never paid for the cost added at
the lexer.

**Transferable lesson:** front-loading work onto the highest-frequency event in
the pipeline (one lex token) to save work on lower-frequency events (map probes)
loses unless the frequency ratio is checked first. See OPTIMIZATION.md §7 for the
negative-results register and §8 for where compile-time effort should go instead.

## Name mangling is canonical (the piece that was kept)

Interning was only *sound* if the `$$` mangling scheme had one byte-producing
definition, so the scattered `format()` sites were unified into
`compiler/support/mangling.{hpp,cpp}`. That was kept: it is perf-neutral and
removes a real drift hazard. New mangled names should go through it (one
exception today: `coroutine_lowering.cpp` formats `__coro_{}$$delete` directly).

**The `$$` spelling is a load-bearing ABI.** Separately from re-derivation, the C
emitter *parses* the byte structure of mangled names to route container methods
to runtime functions: `suffix_after_last_dollar_dollar` splits on the last `$$`,
and `ends_with(fn, "$$get" / "$$get_or" / "$$index" / "$$pop")` and
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

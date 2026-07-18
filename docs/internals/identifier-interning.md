# Identifier Interning (Sym IDs)

Design document for OPTIMIZATION.md §5.1 — interning every source identifier to a
dense `u32` symbol ID (`Sym`) at lex time, so that name-keyed maps hash and
compare integers instead of re-running an FNV-1a byte loop and a `memcmp` on
every probe.

> **⛔ ABANDONED — measured net regression. Do not re-attempt as specified.**
>
> The `Sym` interning approach below was implemented through Phase 0, 1a, 1b,
> 1c/1d, plus §5.2a, then **reverted**. Measured on the 400-module / 257 KLOC
> generated corpus (`roxy_gen --seed=7 --modules=400`, interleaved before/after
> floors — the reliable workload; Lox at ~2 ms is below the noise floor):
>
> - **Full interning was +5.6% total compile time** vs. the pre-`Sym` baseline
>   (parse **+26.7%**, sema **+4.3%**). The driver is the per-identifier-token
>   **hash + robin_map probe at lex** — paid on hundreds of thousands of tokens,
>   far more than the downstream lookups it saves.
> - The cost is **structural and irreducible**: a no-copy `intern_stable` (store
>   the source-buffer view instead of copying bytes) recovered only ~6% of the
>   parse tax, proving the cost is the hash/probe, not the copy.
> - The §5.1 payoff was supposed to come from the **§5.2b `IRInst` union shrink**
>   (locality on the ~68%-of-compile IR walk). But **§5.2a (contiguous `IRInst`
>   pool) measured neutral** — the IR walk is **not `IRInst`-cache-locality-bound**
>   (the bottleneck is the `Vector<IRInst*>` indirection and per-op compute), so
>   §5.2b (a smaller `IRInst`) would not have helped either, and `BumpAllocator`
>   cost is size-independent so there's no allocation-time saving.
>
> **Only Phase P (the canonical mangler, `compiler/mangling.{hpp,cpp}`) was kept**
> — it is perf-neutral and eliminated a real 8-way `$$`-mangling drift hazard,
> independent of interning. See OPTIMIZATION.md §7 for the summary entry, and
> §5.2 / §8 for where compile-time effort should go instead (the IR walk's
> indirection and the allocator traffic, *not* `IRInst` locality).
>
> The remainder of this document is retained as a record of the design that was
> built and why it did not pay — it is **not** a description of existing code
> (only Phase P exists).

**Original goal (not achieved):** replace the raw-`StringView` name representation
that flows through the lexer, AST, sema, IR builder, and lowering with a `Sym{u32}`
hashed by identity and compared by integer equality. Estimated payoff was **5–15%
of total compile time**; the measured result was **−5.6%** (a regression).

```
Source → Lexer[intern] → Token{Sym} → AST{Sym} → Sema{Sym} → IR{Sym} → Lowering → Bytecode
                              │                                              │
                              └────────── one shared InternTable ───────────┘
                                     (Sym → StringView side table for
                                      diagnostics / codegen / LSP)
```

---

## 1. Why this is the highest-ceiling change

Today every name is a raw `StringView` (pointer + length) into the source buffer
(`Token::text()`, `shared/token.hpp:29`). Consequences measured in the profiling
study (OPTIMIZATION.md §2):

- **`std::hash<StringView>` is a byte-wise FNV-1a loop** (`core/string_view.hpp:81-93`)
  re-run on **every** map probe in sema, ir-build, and bc-lower.
- **Every field / method / variant / trait resolution is a linear string scan**
  (`find_field`, `find_variant`, `lookup_method_in_hierarchy`, …) doing a `memcmp`
  per element.
- **`resolve_type_expr` and `primitive_by_name` do keyword `memcmp` cascades**
  (`== "Self"/"List"/"Map"/"Coro"` plus a 13-way primitive cascade) on every type
  annotation.

Interning collapses all of this: a name becomes a 4-byte `Sym`, map hashing
becomes identity, name comparison becomes a `u32 ==`, and keyword checks become
comparisons against pre-interned constant Syms. The FNV cluster and the `memcmp`
cluster both disappear, and `Symbol::name` / `IRInst` name payloads shrink.

---

## 2. The two name populations

The single most important structural fact for scoping this work: names in the
compiler come from two sources, each with one choke-point.

**Population A — source identifiers.** Everything the programmer typed: variable
names, type names, field/method/parameter names. These all enter through
**`Token::text()`** and are copied into ~36 AST fields by the parser. Interning
these once in the lexer makes them arrive pre-interned everywhere downstream.

**Population B — synthetic names.** Mangled / monomorphized names minted *after*
lex that never existed in source: `Box$i32`, `Vec2$$length`, `__lambda_3_env`,
`__tmp7`. These are written into 7 analysis-time AST fields and the `IRInst` name
payloads. They get **no** `Sym` for free. But they all funnel through **four
minting primitives**:

- `IRBuilder::intern_format` (`compiler/ir_builder.hpp:439`)
- `IRBuilder::intern_synthetic_name` (`compiler/ir_builder.hpp:448`)
- `IRBuilder::intern_concat` (`compiler/ir_builder.hpp:465`)
- `GenericInstantiator::mangle_name` (`compiler/generics.cpp:252`)

(The higher-level `mangle_method` / `mangle_constructor` / `mangle_destructor` /
`mangle_module_local` all build on `intern_format`.) Interning **at the mint**
makes a synthetic name downstream-indistinguishable from a source identifier: a
`Sym` with a byte backing in the side table.

So the blast radius is wide for *mechanical* edits but narrow for *semantic*
surface: two choke-points (`Token::text()` and the four minters) plus the three
hard problems in §6.

---

## 3. The `Sym` type and the intern table

### 3.1 `Sym`

```cpp
// roxy/shared/sym.hpp  (roxy_shared library)
struct Sym {
    u32 id;
    constexpr bool operator==(Sym o) const { return id == o.id; }
    constexpr bool operator!=(Sym o) const { return id != o.id; }
    static constexpr Sym none() { return {0}; }   // reserved: empty / absent
};
```

`Sym{0}` is the reserved "none" value — it replaces the `StringView{}` empty
sentinels the parser writes today for absent optional names (`parent_name`,
default-constructor `name`, import `alias`, …). Emptiness checks (`name.empty()`)
become `name == Sym::none()`.

Hashing is **identity** (`hash(Sym) == Sym.id`). Syms are dense sequential
integers issued by a counter, so `tsl::robin_map`'s power-of-two bucketing spreads
them perfectly — consecutive Syms fall in consecutive buckets, with no low-bit
clustering. This is the one case where identity hashing is not just acceptable but
ideal, and it sidesteps the usual "robin_map needs an avalanched hash" concern
(see `feedback_use_vendored_xxhash`). Fallback if Robin Hood probe lengths spike
in practice: a single multiplicative mix.

### 3.2 `InternTable`

```cpp
// roxy/shared/intern_table.hpp  (roxy_shared library)
class InternTable {
public:
    Sym intern(const char* bytes, u32 len);   // dedups; copies bytes on first sighting
    Sym intern(StringView sv) { return intern(sv.data(), sv.size()); }
    StringView text(Sym s) const;             // reverse lookup (the side table)

    // Pre-interned constants (assigned at construction — see §3.4)
    Sym kw_self, kw_List, kw_Map, kw_Coro;
    Sym prim[/* PrimKind count */];           // void, bool, i8..u64, f32, f64, string
    Sym op_method[/* BinaryOp/UnaryOp count */]; // "add", "sub", …
    // …

private:
    BumpAllocator m_arena;                     // owns copied name bytes
    tsl::robin_map<StringView, Sym> m_forward; // keys point into m_arena
    Vector<StringView> m_reverse;              // m_reverse[sym.id] → bytes in m_arena
};
```

**The table copies bytes into its own arena on first intern.** This is deliberate:
token `start` pointers are *not* always into live source (`error_token` points at
a static message literal, `shared/lexer.cpp:177`; the degenerate f-string path
shifts by +1, `:484`), synthetic names live in a *different* arena, and the LSP
replaces its source buffer on every keystroke. Owning the bytes makes a `Sym`
valid for the whole session regardless of where its bytes originated.

### 3.3 Placement and the AOT-linkage constraint

The table lives in **`roxy_shared`** (co-located with `Lexer`/`Token`), which sits
above `roxy_rt` in the CMake link graph.

This placement is load-bearing, not incidental. OPTIMIZATION.md §3.9 records that
moving `std::hash<StringView>` out-of-line into `roxy_core` **broke AOT linkage**:
the C backend links the standalone `roxy_rt` runtime (only `roxy_rt.cpp`,
`slab_allocator.cpp`, `string_intern.cpp`, `vmem_*.cpp`), which pulls in
`std::hash<StringView>` via `rt/string_intern.hpp` but cannot see a `roxy_core`
symbol. The identifier intern table avoids this entirely because:

- It is referenced **only** from `roxy_shared` / `roxy_compiler` translation units,
  never from any of the four `roxy_rt` TUs nor from `roxy_rt.h`.
- It hashes `Sym` by **identity** — it never touches `std::hash<StringView>`.

So no new symbol enters the AOT link closure. **Do not** place `Sym`, the table, or
any of its out-of-line definitions in `roxy_core` or anywhere an rt TU includes.
`rt/string_intern.{hpp,cpp}` is an unrelated *runtime* string-object dedup table —
not reusable here, but a working model of the pattern.

### 3.4 Lifetime and threading

One table **per compile session**, owned by `Compiler`. It is threaded by
reference into every phase that produces or consumes names:

- `Lexer` — a new `InternTable* m_interns` member, set via the constructor. A fresh
  `Lexer` is built per module (`compiler/compiler.cpp:104`, and elsewhere), so the
  table **must** be Compiler-owned and shared; per-lexer ownership would break
  cross-module `Sym` identity (the module registry and generics maps compare names
  across modules).
- `Parser`, `SemanticAnalyzer`, `GenericInstantiator`, `IRBuilder`,
  `BytecodeBuilder`/lowering — all receive the same handle, so synthetic names
  minted in any phase intern into the *same* table. This shared handle is what
  makes "equal bytes → equal `Sym`" hold across phases (see problem §6.3).

Pre-interned constants (§3.2) are assigned once in the table constructor.

---

## 4. Where interning happens

**Source identifiers** — in `Lexer::scan_identifier` (`shared/lexer.cpp:309`),
**only** when `identifier_type()` returns `TokenKind::Identifier` (reserved
keywords already become distinct `TokenKind`s in the trie and are never interned).
The byte range is `[m_source + m_start, length)`. The result is stored in a new
additive `Token` field:

```cpp
struct Token {              // shared/token.hpp
    TokenKind kind;
    SourceLocation loc;
    const char* start;      // KEPT — see below
    u32 length;             // KEPT
    Sym name_sym;           // NEW (Sym::none() for non-identifier tokens)
    union { i64 int_value; f64 float_value; };
    StringView text() const { return {start, length}; }
};
```

`Sym` is **added to** `Token`, not substituted for `start`/`length`. Two reasons:

1. The `>>` / `>>=` token-splitting hack in `try_parse_generic_args` /
   `consume_closing_angle` (`compiler/parser.cpp:2239`, `:2286`) manufactures a
   sub-token by raw pointer arithmetic on `Token.start`. It operates on operator
   tokens, so it is untouched by interning **as long as** operator/literal tokens
   keep `start`/`length`.
2. The LSP CST is lossless and reads `token.text()` in dozens of places
   (hover, rename, completions). Keeping `start`/`length` means those sites need no
   change (see §6.4).

**Synthetic names** — at the four minting primitives (§2). Each already does
"allocate bytes into an arena, return a `StringView`"; the change is to end with
`return m_interns->intern(bytes, len)` and return a `Sym`. This is a ~5-function
edit, not a ~30-call-site edit, because every synthetic name funnels through them.

**Excluded from interning** (must stay `StringView` / raw bytes — interning them
would pollute the symbol space for zero lookup benefit):

- `LiteralExpr.string_value` (`ast.hpp:174`) and `StringInterpExpr.parts`
  (`ast.hpp:308`) — string-literal *content*, escape-processed into the arena.
- `IRInst` `ConstData.string_val` (`ssa_ir.hpp:212`) — a string literal.
- Debug-only names: `IRBlock.name`, `BlockParam.name`, block labels
  (`_pass`/`_fail`), the `arg{}` block-param debug name.

---

## 5. Migration map

| Layer | Sites | Character |
|-------|-------|-----------|
| `Token` | 1 field added | Additive; keep `start`/`length` |
| AST name fields | 43 total (36 source, 7 synthetic) | 2 literal-content fields excluded |
| Parser write sites | ~50 | Nearly all funnel through `Token::text()` → one `intern(token)` helper |
| `StringView`-keyed maps | 57 | See breakdown below |
| Linear name scans | ~11 | Pure win: `memcmp` → `u32 ==` |
| `IRInst` name payloads | 6 union fields | Also enables §5.2b union shrink |
| Synthetic-name minters | 4 primitives (+4 manglers) | Intern-at-mint |

**Map / scan breakdown by outcome:**

- **Pure win** (faster, zero semantic change): `SymbolTable::m_lookup_cache`
  (`symbol_table.hpp:176`) and all define/lookup/pop_scope; `TypeEnv::m_named_types`
  / `m_trait_types` (`type_env.hpp:51-52`); `covered_variants` /
  `variant_field_initialized` (`semantic.cpp:2225`, `:4829`); every linear scan —
  `find_variant`, `find_field`, `find_variant_field`, `lookup_method_in_hierarchy`,
  `lookup_list/map/coro_method`, `resolve_active_type_param`, `unify_type_expr`; the
  `TypeParam` name compare in `TypeEqual` (`types.cpp:267`); the IR-builder scope
  maps `m_local_scopes` / `m_param_is_ptr` / `m_global_indices` / `m_function_refs`
  (`ir_builder.hpp:573-650`) and `OwnershipTracker::m_index_by_name`
  (`ownership_tracker.hpp:95`); lowering's `m_func_indices` (`lowering.hpp:310`).

- **Needs a pre-interned constant:** `primitive_by_name`'s 13-way cascade
  (`types.cpp:476-491`); `resolve_type_expr`'s `Self`/`List`/`Coro`/`Map` compares
  (`semantic.cpp:1238/1254/1266/1278`); the operator-method literals feeding the
  struct-overload path (`semantic.cpp:1808/3492/3502`) and the primitive-operator
  registration arrays (`trait_system.cpp:204-275`); the parser soft keywords
  `move`/`copy`/`borrowed` (`parser.cpp:506/512/2330`) and the `"self"` capture
  name (`parser.cpp:516/521`).

- **Needs the original bytes (store `Sym`, resolve via `text()`):** all
  `error`/`error_fmt` name interpolation (~197 sites in `semantic.cpp`); the
  generics registries keyed on mangled names (`generics.hpp:187-229`); codegen byte
  fields threaded through sema (`Symbol::imported_func.module_name`/`original_name`
  `symbol_table.hpp:81-82`, `StructTypeInfo::module_name` `types.hpp:166`, container
  native-call names `types.hpp:221-239`); the C backend `emit_mangled_name` and its
  `field_name == "__zero"_sv` compare (`c_emitter.cpp:1437`).

- **No change (already pointer/int-keyed):** `TypeCache::m_interned` (hash-cons on
  component/`decl` pointers, `types.hpp:589`); `m_primitive_methods` /
  `m_primitive_traits` (`u8`-keyed); `m_move_states` / `by_symbol` (`Symbol*`-keyed);
  `m_resolving_structs` (`Type*`); the dense operator tables `m_primitive_binary_ops`
  / `m_primitive_unary_ops` (already name-free — §3.5 landed).

**Special cases:**

- `ImportDecl.module_path` (`ast.hpp:734`) is a *dotted* path (`a.b.c`) built by
  concatenation (`parser.cpp:2044`), not a single token. Either intern the joined
  string as one `Sym`, or restructure to `Span<Sym>` of segments.
- `Span<StringView>` fields — `WhenCase.case_names` (`ast.hpp:563`),
  `WhenCaseFieldDecl.case_names` (`ast.hpp:683`) — become `Span<Sym>`.

---

## 6. Hard problems

Three problems are genuinely load-bearing; the rest of the migration is mechanical.

### 6.1 The mangling scheme is re-derived in eight places — unify it first

This is the highest-severity problem, and it is **larger than an early read
suggested**. `IRBuilder::mangle_method` / `mangle_constructor` / `mangle_destructor`
(`ir_builder.cpp:966-980`, `:1169-1174`) look canonical, but the same
`"{}$$…"` scheme is independently re-`format()`'d at **eight sites across five
files**, each with its own literal that must byte-match the others:

| Site | Literal | Purpose |
|------|---------|---------|
| `ir_builder.cpp:966-975`, `:1169-1174` | `"{}$${}"` / `"{}$$new…"` / `"{}$$delete…"` | the nominal canonical manglers |
| `lowering.cpp:3358` | `"{}$$delete"` | VM: default-dtor → bc func index (`lookup_destructor_index`) |
| `ir_builder_expr.cpp:1328` | `mangle_method` + scan | method → fn index (`find_method_fn_index`) |
| `semantic.cpp:1698` | `"{}$${}"` | sema re-mangles a method name |
| `coroutine_lowering.cpp:279` | `"{}$$delete"` | coro lowering: promoted-field dtor |
| `c_emitter.cpp:443` | `"{}$$delete"` | C: struct dtor |
| `c_emitter.cpp:2626` | `"{}$$delete"` | C: closure-env dtor |
| `c_emitter.cpp:3163`, `:3186` | `"{}$${}"` / `"{}$$delete"` | C: method / dtor (header emit) |

Today these agree only by duplicated convention: everyone formats the same bytes
and compares bytes, so drift would already be a bug — it is simply latent. **Under
interning the requirement becomes hard:** each site must resolve to the *same*
`Sym`, which holds only if the bytes are produced identically and interned into the
one shared table (§6.3). Eight hand-copied format strings cannot be trusted to stay
identical.

**Resolution (in order):**

1. **Unify to one canonical mangler — as a standalone refactor, before any `Sym`
   work.** Move the `$$` scheme into free functions in a shared header
   (`compiler/mangling.hpp`) and route all eight sites through them. This is
   valuable *independently of interning* (it removes eight-way format-string drift
   that exists today) and is a hard prerequisite: interning is only sound with a
   single byte-producing definition. Land and measure it green on its own.
2. **After interning, the manglers return `Sym`.** Equal inputs then yield an equal
   `Sym` by construction, so all eight sites converge automatically.
3. **Carry the `Sym` on `DestructorInfo` / `MethodInfo`** (`types.hpp:78`, `:85`;
   add a `Sym mangled` field, set once where the IR function is named). The two hot
   lookup sites — `lookup_destructor_index` and `find_method_fn_index` — then read
   the exact `Sym` the function was named with, eliminating re-derivation *and* the
   round-trip mangle (§6.2). This also retires the `lowering.cpp:3358` hard-coded
   `"{}$$delete"`, which silently assumes the default (unnamed) destructor form —
   sound today only because `DropKind::CallDtor` always targets the default dtor
   (`lowering.cpp:3387`, `types.cpp:107/140/159`), but a second copy of the scheme
   regardless.

A divergence here mis-resolves a call or produces a double-free — the same failure
class that sank the §3.6 `Param::resolved_type` attempt (OPTIMIZATION.md §7.5).

**Guard:** in debug builds, assert `intern(text(s)) == s` for every mangled `Sym`
at construction — a round-trip check that catches any un-unified site immediately.

### 6.1b The C backend parses mangled names by `$$` structure (a separate, non-divergence hazard)

Distinct from re-derivation: the C emitter *reads the byte structure* of mangled
names to route container methods to runtime functions. `suffix_after_last_dollar_dollar`
(`c_emitter.cpp:3457`) splits on the last `$$`; `ends_with(fn, "$$get"/"$$index"/"$$pop")`
(`:310`), the `"$$resume"` suffix (`:2563`), and `ends_with("$$delete")` all pattern-match
the spelling. These do **not** risk divergence, but they mean the `$$` spelling is a
**load-bearing ABI the C backend parses**. Under interning they operate on
`InternTable::text(sym)` and keep their existing string logic unchanged — they never
touch the `Sym`. Consequence: the mangling byte scheme is frozen; it cannot be
replaced by a Sym-only encoding, and the side table is mandatory at codegen (not
merely for diagnostics).

### 6.2 Nested manglers round-trip through bytes

`mangle_method(struct_sym, method_sym)` composes a name whose `struct_sym`
component may itself be a mangled generic-instance name (`Box$i32` →
`Box$i32$$get`). With `Sym` components the mangler must
`text(struct_sym)` + `text(method_sym)` → concatenate bytes → `intern`. A
`Sym`-in/`Sym`-out that necessarily round-trips through raw bytes. Unavoidable, but
localized to the (now unified — §6.1) mangler functions, and avoided entirely at the
two hot lookup sites once they read a carried `Sym` (§6.1 resolution step 3).

### 6.3 One table, all phases

For §6.1 and §6.2 to be sound, sema/generics (`mangle_name`), IR build (the four
primitives), and lowering (`lowering.cpp:3358`) must all intern into the **same**
table — equal bytes yield an equal `Sym` only within one table. Today these phases
merely share a `BumpAllocator`; the migration threads one `InternTable&` handle
through `GenericInstantiator`, `IRBuilder`, and `BytecodeBuilder`/lowering (§3.4).

### 6.4 LSP lifetime

The LSP shares the exact `roxy_shared` `Lexer`/`Token`, so interning reaches it for
free — but its lifetime model is the opposite of the batch compiler's. The server
re-lexes and re-parses into a **throwaway arena on every keystroke and every
feature call** (`server.cpp:244`, `:332`, and per-handler), and its *persistent*
indices — `GlobalIndex` and `SymbolIdentity` — key on **owned `String`s** that
outlive any single parse (`global_index.hpp:95`, `server.cpp:1960`).

**Resolution: the table stays per-parse, and `Sym` must not escape into persistent
LSP state.** Those indices keep extracting bytes via `token.text()` /
`InternTable::text()` and copying into owned `String`s, exactly as today. Because
`Sym` is *additive* to `Token`, every LSP `token.text()` call site is untouched.
This preserves the current LSP architecture. (The alternative — a persistent,
string-owning, server-lifetime table so `Sym` can key the indices — is a larger
change and is not needed for the compile-time goal. It can be revisited if the LSP
later wants Sym-keyed cross-file indices.)

---

## 7. Phased plan

Each phase is independently measurable per OPTIMIZATION.md's discipline (interleave
before/after builds; trust the total, not the isolated phase timer).

**Phase P — Canonical mangler (prerequisite; no `Sym` yet). ✅ Landed.** The eight
re-derivation sites (§6.1) now route through free functions in
`compiler/mangling.{hpp,cpp}` (`mangle_method` / `mangle_constructor` /
`mangle_destructor` / `mangle_module_local`, plus `_owned` String variants for
allocator-less sites); each `$$` format literal is defined exactly once.
`IRBuilder::mangle_*` members delegate to them, so their callers are unchanged.
Verified perf-neutral (Lox `--repeat=3000`: 2.20 ms) and green across the VM and C
backends. This was a hard prerequisite — interning is unsound while the `$$` scheme
has independent definitions.

**Phase 0 — Infrastructure (expect a small standalone cost).** Add `Sym` +
`InternTable` in `roxy_shared`; pre-intern the constant set; add `Token.name_sym`
and populate it in `scan_identifier`; thread the table handle through all phases.
Nothing keys on `Sym` yet. This *adds* a hash + dedup per identifier token with no
downstream removal, so measure it and accept a small regression — it is recouped in
Phase 1.

**Phase 1 — Sema cluster (first net win).** Migrate `SymbolTable`, the `TypeEnv`
maps, the linear scans, and `primitive_by_name` / `resolve_type_expr` to `Sym`.
This entails flipping the AST *source* name fields (`IdentifierExpr.name`,
`TypeExpr.name`, `GetExpr.name`, field/param/struct names) to `Sym` in the same
phase — they must arrive pre-interned, or the FNV hash reappears at each
`define`/`lookup`. **This is the checkpoint where the plan first pays**; if it does
not show a net win after Phase 0's cost, stop before the larger Phase 2 (§8).

**Phase 2 — IR build + generics + synthetic names.** Intern-at-mint in the four
primitives (now the unified manglers); migrate the IR scope maps, the `IRInst` name
payloads, the generics registries, and lowering's `m_func_indices`. Add the carried
`Sym` on `DestructorInfo`/`MethodInfo` (§6.1 step 3) and the debug round-trip guard.
Delivers the ir-build win and sets up the union shrink.

**Phase 3 — C backend + LSP boundary.** Point `emit_mangled_name` and the
field-name literal compares at `InternTable::text()`; enforce the "Syms don't
escape" rule in the LSP indices (§6.4).

**Phase 4 (follow-on) — §5.2b union shrink.** With IR names as `Sym`,
`CallExternalData` / `ClosureData` drop from 3× 16-byte views to 3× 4-byte Syms,
taking `IRInst` from 80 → ~56 B. Separately measurable; benefits every IR-walking
phase (~50% of a large compile).

---

## 8. Risks and kill-criteria

- **Divergent re-derivation (§6.1)** is the highest-severity risk: a wrong or
  duplicate `Sym` mis-resolves a call or double-frees. Mitigate by carrying the
  `Sym` (not re-deriving) and by the debug round-trip assertion. This is the same
  failure class as the reverted §3.6 caching attempt — treat name identity as a
  strict cross-phase contract.
- **Kill-criterion:** if Phase 1 does not show a measurable net win on the large
  generated corpus after absorbing Phase 0's intern-at-lex cost, the amortization
  assumption is wrong — stop before Phase 2.
- **Blast-radius creep:** the 43 AST fields + 57 maps are a large mechanical diff.
  Land per-cluster (§7), keeping the tree green at each phase; do not attempt a
  big-bang swap.

---

## 9. Reference anchors

Foundation: `shared/token.hpp:16`, `shared/lexer.hpp:30`, `shared/lexer.cpp:309`
(`scan_identifier`), `core/string_view.hpp:81` (FNV hash).
Parser: `compiler/parser.cpp` (~50 `Token::text()` write sites),
`:2239`/`:2286` (`>>` splitter).
AST: `compiler/ast.hpp` (43 name fields; literal-content `:174`, `:308`).
Sema: `compiler/symbol_table.hpp:176`, `compiler/type_env.hpp:51`,
`compiler/types.cpp:476` (`primitive_by_name`), `compiler/semantic.cpp:1201`
(`resolve_type_expr`).
IR: `compiler/ir_builder.hpp:438-473` (minters), `:573-650` (scope maps),
`compiler/ssa_ir.hpp:212-265` (name payloads), `compiler/generics.cpp:252`
(`mangle_name`), `compiler/lowering.hpp:310`, `compiler/lowering.cpp:3358`
(re-derive), `compiler/ir_builder_expr.cpp:1323` (`find_method_fn_index`).
Mangling (§6.1): canonical `compiler/ir_builder.cpp:966-980`, `:1169-1174`;
re-derived at `compiler/lowering.cpp:3358`, `compiler/ir_builder_expr.cpp:1328`,
`compiler/semantic.cpp:1698`, `compiler/coroutine_lowering.cpp:279`,
`compiler/c_emitter.cpp:443/2626/3163/3186`; `$$`-structure parsing at
`compiler/c_emitter.cpp:310/2563/3457` (§6.1b). Carried-`Sym` homes:
`compiler/types.hpp:78` (`DestructorInfo`), `:85` (`MethodInfo`).
Codegen: `compiler/c_emitter.cpp` (`emit_mangled_name`, `:1437`).
LSP: `lsp/lsp_parser.hpp:14`, `lsp/global_index.hpp:95`, `lsp/server.cpp:244/1960`.
Linkage: `CMakeLists.txt` (`roxy_core`/`roxy_shared`/`roxy_rt` graph), OPTIMIZATION.md §3.9.

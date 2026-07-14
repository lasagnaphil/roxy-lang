# LSP Server

The Roxy LSP server provides IDE features — diagnostics, completions, hover, go-to-definition, find references, and rename — over JSON-RPC on stdin/stdout. It uses a **map-reduce** architecture: per-file indexing produces lightweight stubs, those stubs reduce into a global index, and full semantic analysis runs lazily per function.

**Implemented (Phases 1–7):** error-recovering CST parser, per-file indexing + document symbols, global index + go-to-definition, completions, hover, semantic diagnostics, find references, rename.

**Pending:**
- **Phase 8 — Full semantic analysis:** replace the lightweight string-based `LspTypeResolver` with the compiler's `Semantic`/`TypeCache`/`TypeEnv` pass for precise generic inference, trait dispatch, and chained-expression completions.
- **Phase 9 — Polish:** signature help, code actions, workspace symbols, semantic token highlighting, threading/cancellation, performance tuning.

## Architecture: Map-Reduce

Following [Three Architectures for a Responsive IDE](https://rust-analyzer.github.io/blog/2020/07/20/three-architectures-for-responsive-ide.html):

1. **Map (per-file indexing):** Each file is parsed and indexed independently into lightweight "stubs" — top-level declarations with unresolved types. Cheap (syntax-only) and parallelizable.
2. **Reduce (global index):** All stubs merge into a single index mapping qualified names to types, functions, methods, and traits.
3. **Lazy analysis:** Full type checking runs on demand, per function, against the global index.

```
                  ┌──────────────────────────────────────────┐
                  │              LSP Server                   │
                  │                                          │
  File Edit ──────┤  ┌─────────────┐    ┌───────────────┐   │
                  │  │  Per-File    │    │  Global       │   │
                  │  │  Indexer     │───>│  Index        │   │
                  │  │  (parallel)  │    │  (stubs)      │   │
                  │  └─────────────┘    └───────┬───────┘   │
                  │                             │           │
  LSP Request ────┤                     ┌───────▼───────┐   │──── LSP Response
                  │                     │  Lazy          │   │
                  │                     │  Analyzer      │   │
                  │                     │  (per-function) │   │
                  │                     └───────────────┘   │
                  └──────────────────────────────────────────┘
```

### Why Map-Reduce

Roxy satisfies the prerequisites: an explicit `import`/`from` module system gives clear file boundaries; no macros or codegen create cross-file declarations; declarations are self-describing (`fun Point.eq() for Eq` states its associations syntactically); and module-qualified names (`math.sin`) suffice for cross-module lookup. The header-based approach doesn't fit (no headers), and the query-based incremental approach (salsa/rust-analyzer) is overkill — no proc macros, no orphan trait impls, simple module-to-file mapping.

The key payoff: **most edits are inside function bodies and don't touch the index.** Only structural changes (adding/removing/renaming declarations, changing field types) trigger an index update.

## Error-Recovering Parser

The compiler parser is fail-fast (stops on first error). The LSP parser must always produce a tree, even for incomplete or malformed code, so it is a **separate implementation** sharing only the lexer and token definitions — keeping recovery complexity out of the compiler's fast path.

| Aspect | Compiler Parser | LSP Parser |
|--------|----------------|------------|
| Error handling | Fail-fast (one error) | Error recovery (always produces tree) |
| Tree format | Lossy AST (discards trivia) | Lossless CST (preserves whitespace, comments) |
| Allocation | BumpAllocator | Arena per parse (replaceable) |
| Output | `Program*` (AST nodes) | `SyntaxTree` (CST nodes) |

### Concrete Syntax Tree (CST)

The CST preserves all source information using a single flat `SyntaxNode` struct (no inheritance), each carrying a `SyntaxKind`, a byte-offset `TextRange`, parent/children links, and a `Token` for leaves. See `lsp/syntax_tree.hpp`.

- **Leaf nodes** have `kind` in the terminal range (e.g. `TokenIdentifier`) and `token` set.
- **Interior nodes** have `kind` in the non-terminal range (e.g. `NodeVarDecl`) and children populated.
- **Error nodes** have `kind == SyntaxKind::Error` and `error_message` set.

### Recovery Strategies

The parser uses three strategies depending on context:

1. **Statement-boundary synchronization** — on an unexpected token inside a statement, emit an error node and skip tokens until a sync point (`;`, `}`, or a statement-starting keyword like `if`, `while`, `var`, `fun`).
2. **Synthetic token insertion** — when a specific token is expected but missing, insert a zero-width synthetic token of the expected kind at the current position, record a diagnostic, and continue.
3. **Bracket-aware skipping** — when recovering inside brackets, skip to the matching close bracket while tracking nesting depth.

**Forward-progress invariant.** The paramount property of this parser is that it *always terminates* — a hang would freeze the editor. Two subtleties break this if unguarded and are worth calling out because fuzzing (`tests/fuzz/fuzz_lsp_parser`) found both:

- `synchronize_to_statement_boundary()` stops *at* a statement-start keyword (e.g. `return`) **without consuming it**, so the enclosing statement loop can re-parse it. A recovery loop that `continue`s straight back into synchronization therefore spins forever on such a token. The `when`-statement and tagged-union `when`-field case loops guard against this by advancing one token whenever a recovery step consumed nothing.
- `parse_primary()` returns an error node **without consuming** on a token that cannot start an expression (`}`, `"`, `,`, `::`, …). The unbounded declaration loops (`parse_program`, `parse_block_stmt`) therefore carry a forward-progress backstop: if a full `parse_declaration()` attempt consumed no tokens, skip one so the loop cannot stall.

The rule for any new unbounded recovery loop: **guarantee it consumes at least one token per iteration** (compare `m_current.loc.offset` before/after and `advance()` if unchanged). Relatedly, the `when`-statement discriminant is parsed as a struct-literal-suppressed expression (like the compiler parser), so member-access discriminants such as `when self.kind` parse cleanly instead of falling into non-progressing recovery.

### CST-to-AST Lowering

For semantic analysis, the CST lowers to the compiler's existing AST format (`lsp/cst_lowering.hpp`). Error nodes lower to `nullptr` or sentinel AST nodes. The semantic analyzer's existing `Error` type kind is extended so that expressions/statements containing error nodes propagate `Error` types without cascading false diagnostics.

## Per-File Index (Stubs)

The indexer runs after the error-recovering parse and extracts top-level declarations into lightweight stubs (`FileStubs`, holding vectors of `StructStub`, `EnumStub`, `FunctionStub`, `MethodStub`, `ConstructorStub`, `DestructorStub`, `TraitStub`, `TraitImplStub`, `ImportStub`, `GlobalVarStub`; see `lsp/indexer.hpp`). Each stub captures the declaration's **syntax** — name, ranges (full and name-token), visibility, params/fields with *unresolved* `TypeExpr*` types, generics, trait associations — without resolving types. A `content_hash` per file supports change detection.

| Declaration | Extracted information |
|-------------|----------------------|
| `struct Point { x: i32; }` | Name, fields (names + unresolved types), parent, when clauses, generics |
| `enum Color { Red, Green }` | Name, variants (names + values) |
| `fun add(a: i32): i32 { ... }` | Name, params (names + unresolved types), return type, visibility |
| `fun Point.sum(): i32 { ... }` | Struct name, method name, params, return type |
| `fun Point.eq(o: Point): bool for Eq` | Struct name, method name, trait name, trait type args |
| `fun new Point(x: i32) { ... }` | Struct name, constructor name, params |
| `fun delete Point() { ... }` | Struct name, destructor name |
| `trait Printable;` | Trait name, parent trait, type params |
| `import math;` / `from math import sin;` | Module path, imported symbols |
| `var global_count: i32 = 0;` | Name, unresolved type |

The indexer does **not** look inside function bodies — bodies are analyzed lazily on demand.

## Global Index

The global index (`GlobalIndex`, see `lsp/global_index.hpp`) merges all per-file stubs into unified lookups: module→file mapping, qualified-name→declaration maps (`find_struct`, `find_enum`, `find_trait`, `find_function`), struct→associated-declaration maps (methods, constructors, destructors, trait impls), plus cross-cutting queries (`find_trait_implementations`, `find_subtypes`). It also tracks type information needed by lazy analysis: struct parents, field types, field-default flags (`field_has_default()`), and function/method return types. All maps are `tsl::robin_map`.

### Update on file edit

1. Re-lex and re-parse the file (error-recovering parser).
2. Re-run the indexer to produce new `FileStubs`.
3. Compare against the old stubs by content hash.
4. If top-level declarations changed: `update_file()` rebuilds the file's index entries and invalidates dependent caches.
5. If only function bodies changed: no index update (bodies aren't indexed).

## Lazy Semantic Analysis

Full type checking runs on demand, not eagerly for the whole project. When the user requests completions, hover, or diagnostics inside a function body, `LspTypeResolver` (`lsp/lsp_type_resolver.hpp`) processes that function against the global index. It mirrors the compiler's passes on a subset:

**Phase 1 — Resolve types from index (cached):** On first use (or after index invalidation), resolve all struct/enum/trait stubs into types, populating fields, enum variants, method signatures, and trait definitions. Equivalent to compiler passes 1–2 but driven from stubs. Re-runs only when the global index changes.

**Phase 2 — Analyze function body (per-request):** Lower the function body CST to AST and run the compiler's expression/statement analysis (type checking, inference, symbol resolution) in a temporary scope, collecting diagnostics, resolved types, and symbol references. Equivalent to compiler pass 3, scoped to one function.

```
                    ┌─────────────────────────────────┐
                    │         Cache Layers             │
  Index change ────>│  Layer 1: Resolved Types         │ (invalidated on index change)
                    │    all struct/enum types,         │
                    │    methods, traits                │
  Body edit ───────>│  Layer 2: Per-Function Analysis   │ (invalidated on body edit)
                    │    diagnostics, resolved exprs,   │
                    │    local symbol table             │
                    └─────────────────────────────────┘
```

Layer 1 depends only on top-level declarations, so it is stable across most edits. Layer 2 is rebuilt per-function on body edits; since functions are typically small, this is fast.

> The lazy analyzer is currently a lightweight string-based resolver. Phase 8 will replace it with the compiler's full `Semantic`/`TypeCache`/`TypeEnv` pass.

## LSP Features

- **Diagnostics** (`publishDiagnostics`) — two-tier: syntax diagnostics report immediately from the error-recovering parser on every keystroke; semantic diagnostics run lazy analysis after a debounce. Semantic checks cover unresolved identifiers/functions/types, unknown type annotations, unresolved field access / method calls / enum variants on known types, wrong argument counts (through inheritance), struct-literal field validation, missing required fields, named-constructor validation, duplicate parameter names, and var/return type mismatches — with cascade prevention so an unknown type doesn't spawn downstream errors.
- **Completions** (`completion`) — triggered by `.`, `::`, or a partial identifier. `.` enumerates fields + methods (including inherited and trait methods); `::` lists enum variants; a bare identifier lists locals, globals, functions, structs, enums, traits, and imports; a type-annotation position lists known types and `uniq`/`ref`/`weak` modifiers. Detail strings include signatures.
- **Hover** (`hover`) — resolves the CST node at the cursor to its type/signature: variable type, function/method signature (with owning struct and trait), struct field, type definition, or enum variant.
- **Go-to-Definition** (`definition`) — locals/params resolve within the current function scope; functions, types, methods, fields, and globals resolve via the global index, returning the declaration's `name_range`; imported symbols follow the import to the source module.
- **Find References** (`references`) — lazy approach: scan all file CSTs for matching identifier tokens, then filter by semantic context (a `SymbolCategory`/`SymbolIdentity` system disambiguates per category; method/field references match by resolved receiver type; locals/params are scoped to the enclosing function). Honors `includeDeclaration`.
- **Document Symbols** (`documentSymbol`) — read directly from per-file stubs; no semantic analysis.
- **Rename** (`rename`) — identify the symbol (go-to-def logic), find all references, and emit a cross-file `WorkspaceEdit`; method/constructor renames also update mangled-name references.

## Threading Model

```
                    ┌─────────────────────────────────┐
                    │         Main Thread              │
                    │  (LSP message dispatch)          │
                    │  didOpen/didChange ──> Queue     │
                    │  completion/hover  ──> Queue     │
                    └────────────┬────────────────────┘
                    ┌────────────▼────────────────────┐
                    │      Analysis Thread Pool        │
                    │  Worker 1: Re-index file A       │
                    │  Worker 2: Analyze function body  │
                    │  Worker 3: Re-index file B       │
                    └──────────────────────────────────┘
```

The main thread receives LSP messages and dispatches to workers; workers run indexing (parallelizable per file) and lazy analysis. On a new edit, in-progress analysis for that file is cancelled and restarted, with a generation counter to discard stale results. Requests are prioritized: `completion`/`signatureHelp` (high, user typing) preempt `hover`/`definition` (medium, navigating), which preempt `references`/`rename`/`diagnostics` (low, background). Threading/cancellation is part of pending Phase 9.

## Reused Components

The LSP server shares existing compiler infrastructure rather than duplicating it:

| Component | Reuse |
|-----------|-------|
| **Lexer** (`shared/lexer.hpp`) | As-is — all token types, positions, f-strings |
| **Token kinds** (`shared/token_kinds.hpp`) | As-is — single source of truth for keywords/operators |
| **TypeCache** (`compiler/types.hpp`) | As-is — interning, method lookup, trait checking |
| **TypeEnv** (`compiler/type_env.hpp`) | With invalidation support for index updates |
| **ModuleRegistry** (`compiler/module_registry.hpp`) | As-is — native + script module lookups |
| **NativeRegistry** (`vm/binding/registry.hpp`) | As-is — built-in signatures for completions |
| **GenericInstantiator** (`compiler/generics.hpp`) | Template registration reusable; instantiation may need per-request arenas |

## Files

| File | Purpose |
|------|---------|
| `include/roxy/lsp/syntax_tree.hpp` | CST node types, `SyntaxKind`, `TextRange` |
| `include/roxy/lsp/lsp_parser.hpp` | Error-recovering parser producing CST |
| `include/roxy/lsp/indexer.hpp` | Per-file stub extraction |
| `include/roxy/lsp/global_index.hpp` | Merged index: qualified lookups, type info, field defaults, param counts |
| `include/roxy/lsp/cst_lowering.hpp` | CST-to-AST lowering for function bodies |
| `include/roxy/lsp/lsp_type_resolver.hpp` | Lazy per-function type resolution + semantic diagnostics |
| `include/roxy/lsp/transport.hpp` | JSON-RPC over stdin/stdout |
| `include/roxy/lsp/server.hpp` | Request dispatch, document state, feature handlers |
| `include/roxy/lsp/protocol.hpp` | LSP protocol types (Position, Range, etc.) |
| `src/roxy/lsp/*.cpp` | Implementations |
| `tests/unit/test_lsp_parser.cpp` | CST parsing, error recovery (15 cases) |
| `tests/unit/test_global_index.cpp` | Index CRUD, qualified lookups (21 cases) |
| `tests/unit/test_lsp_type_resolver.cpp` | Variable scope, type resolution (15 cases) |
| `tests/unit/test_lsp_completion.cpp` | Dot, `::`, bare, type completions (16 cases) |
| `tests/unit/test_lsp_hover.cpp` | Hover on vars, functions, fields, types (14 cases) |
| `tests/unit/test_lsp_semantic_diagnostics.cpp` | Unresolved symbols, type mismatches, missing fields (31 cases) |
| `tests/unit/test_lsp_references.cpp` | Find references, rename, symbol categories (15 cases) |
| `tests/fuzz/fuzz_lsp_parser.cpp` | Coverage-guided libFuzzer target (see `tests/fuzz/README.md`) |
| `tests/unit/test_fuzz_regression.cpp` | Replays the seed corpus + `examples/` through the parser harnesses each test run |

The `roxy_lsp` library depends on `roxy_shared` (lexer, tokens) and `roxy_compiler` (AST types for CST-to-AST lowering).

# LSP Server

The Roxy LSP server provides IDE features — diagnostics, completions, hover, go-to-definition, find references, and rename — over JSON-RPC on stdin/stdout. It uses a **map-reduce** architecture: per-file indexing produces lightweight stubs, those stubs reduce into a global index, and full semantic analysis runs lazily per function.

**Implemented (Phases 1–7):** error-recovering CST parser, per-file indexing + document symbols, global index + go-to-definition, completions, hover, semantic diagnostics, find references, rename.

**Pending:**
- **Phase 8 — Full semantic analysis:** `LspAnalysisContext` already runs the compiler's real `SemanticAnalyzer`/`TypeCache`/`TypeEnv` over the workspace; what remains is routing *every* feature through it (several still answer from the string-typed global index) for precise generic inference, trait dispatch, and chained-expression completions.
- **Phase 9 — Polish:** signature help, code actions, workspace symbols, semantic token highlighting, threading/cancellation, performance tuning.

The server is **single-threaded** today: requests are handled in order on the read loop. Threading and cancellation are Phase 9.

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

The indexer runs after the error-recovering parse and extracts top-level declarations into lightweight stubs (`FileStubs`, holding vectors of `StructStub`, `EnumStub`, `FunctionStub`, `MethodStub`, `ConstructorStub`, `DestructorStub`, `TraitStub`, `ImportStub`, `GlobalVarStub`; see `lsp/indexer.hpp`). Each stub captures the declaration's **syntax** — name, ranges (full and name-token), visibility, params/fields with types as *unresolved* `TypeRef` strings, generics, trait associations — without resolving types. Trait implementations are recorded on the `MethodStub` (its `trait_name`), not as a separate stub kind.

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

The global index (`GlobalIndex`, see `lsp/global_index.hpp`) merges all per-file stubs into unified lookups keyed by name: `find_struct` / `find_enum` / `find_trait` / `find_function` / `find_global`, and struct-qualified `find_method` / `find_constructor` / `find_field`, each returning a `SymbolLocation` (uri + ranges). `find_any` collects every category for one name. It also caches the string-typed information features need without full analysis: struct parents (`find_struct_parent`), field and return types, signatures, parameter counts, field-default flags (`field_has_default`), and `for_each_*` enumeration for completions. All maps are `tsl::robin_map`.

### Update on file edit

1. Re-lex and re-parse the file (error-recovering parser).
2. Re-run the indexer to produce new `FileStubs`.
3. `update_file()` replaces that file's index entries (`remove_file` + re-insert).
4. Function bodies are not indexed, so body-only edits change nothing in the index.

## Lazy Semantic Analysis

Full type checking runs on demand, not eagerly for the whole project. `LspAnalysisContext` (`lsp/lsp_analysis_context.hpp`) owns the persistent type state — a `BumpAllocator`, `TypeEnv`, `ModuleRegistry`, the builtin `NativeRegistry`, and the declaration-level `SymbolTable` — and splits the compiler's passes in two:

**`rebuild_declarations(files)` — compiler passes 0–2, whole workspace (cached):** reset the type state, lower every file's CST to a declaration AST, and run import resolution, type collection, and member/signature resolution. Bumps `declaration_version()`; re-runs only when top-level declarations change.

**`analyze_function_body(fn_cst, allocator)` — compiler pass 3, one function (per-request):** lower that function's CST to a fresh AST and run the real `SemanticAnalyzer` over its body, returning a `BodyAnalysisResult` (lowered `Decl*`, populated `SymbolTable*`, `SemanticError`s). A fresh AST per call is required — analysis is single-shot and rewrites the tree it walks (see the annotation-contract block in `ast.hpp`). Helpers on top of it (`collect_local_variables`, `resolve_cst_expr_type`, `type_to_string`) answer the feature handlers' questions.

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

> Not every feature is on this path yet — several answer from the string-typed `GlobalIndex` instead of resolved `Type*`s. Routing them all through `LspAnalysisContext` is Phase 8.

## LSP Features

- **Diagnostics** (`publishDiagnostics`) — two-tier: syntax diagnostics report immediately from the error-recovering parser on every keystroke; semantic diagnostics run lazy analysis after a debounce. Semantic checks cover unresolved identifiers/functions/types, unknown type annotations, unresolved field access / method calls / enum variants on known types, wrong argument counts (through inheritance), struct-literal field validation, missing required fields, named-constructor validation, duplicate parameter names, and var/return type mismatches — with cascade prevention so an unknown type doesn't spawn downstream errors.
- **Completions** (`completion`) — triggered by `.`, `::`, or a partial identifier. `.` enumerates fields + methods (including inherited and trait methods); `::` lists enum variants; a bare identifier lists locals, globals, functions, structs, enums, traits, and imports; a type-annotation position lists known types and `uniq`/`ref`/`weak` modifiers. Detail strings include signatures.
- **Hover** (`hover`) — resolves the CST node at the cursor to its type/signature: variable type, function/method signature (with owning struct and trait), struct field, type definition, or enum variant.
- **Go-to-Definition** (`definition`) — locals/params resolve within the current function scope; functions, types, methods, fields, and globals resolve via the global index, returning the declaration's `name_range`; imported symbols follow the import to the source module.
- **Find References** (`references`) — lazy approach: scan all file CSTs for matching identifier tokens, then filter by semantic context (a `SymbolCategory`/`SymbolIdentity` system disambiguates per category; method/field references match by resolved receiver type; locals/params are scoped to the enclosing function). Honors `includeDeclaration`.
- **Document Symbols** (`documentSymbol`) — read directly from per-file stubs; no semantic analysis.
- **Rename** (`rename`) — identify the symbol (go-to-def logic), find all references, and emit a cross-file `WorkspaceEdit`; method/constructor renames also update mangled-name references.

## Threading Model (planned — Phase 9)

The server is single-threaded today; every request is handled synchronously on the read loop. The intended shape: a main thread that receives LSP messages and dispatches to a worker pool running indexing (parallelizable per file) and lazy analysis, with in-progress analysis for an edited file cancelled and restarted behind a generation counter, and requests prioritized `completion` > `hover`/`definition` > `references`/`rename`/`diagnostics`.

## Reused Components

The LSP server shares existing compiler infrastructure rather than duplicating it:

| Component | Reuse |
|-----------|-------|
| **Lexer** (`shared/lexer.hpp`) | As-is — all token types, positions, f-strings |
| **Token kinds** (`shared/token_kinds.hpp`) | As-is — single source of truth for keywords/operators |
| **TypeCache** (`compiler/types/types.hpp`) | As-is — interning, method lookup, trait checking |
| **TypeEnv** (`compiler/types/type_env.hpp`) | With invalidation support for index updates |
| **ModuleRegistry** (`compiler/driver/module_registry.hpp`) | As-is — native + script module lookups |
| **NativeRegistry** (`vm/binding/registry.hpp`) | As-is — built-in signatures for completions |
| **GenericInstantiator** (`compiler/types/generics.hpp`) | Template registration reusable; instantiation may need per-request arenas |

## Files

| File | Purpose |
|------|---------|
| `include/roxy/lsp/syntax_tree.hpp` | CST node types, `SyntaxKind`, `TextRange` |
| `include/roxy/lsp/lsp_parser.hpp` | Error-recovering parser producing CST |
| `include/roxy/lsp/indexer.hpp` | Per-file stub extraction |
| `include/roxy/lsp/global_index.hpp` | Merged index: qualified lookups, type info, field defaults, param counts |
| `include/roxy/lsp/cst_lowering.hpp` | CST-to-AST lowering for declarations and function bodies |
| `include/roxy/lsp/lsp_analysis_context.hpp` | Persistent type state + declaration rebuild + per-body analysis |
| `include/roxy/lsp/transport.hpp` | JSON-RPC over stdin/stdout |
| `include/roxy/lsp/server.hpp` | Request dispatch, document state, feature handlers |
| `include/roxy/lsp/protocol.hpp` | LSP protocol types (Position, Range, etc.) |
| `src/roxy/lsp/*.cpp` | Implementations |
| `tests/unit/test_lsp_parser.cpp` | CST parsing, error recovery |
| `tests/unit/test_indexer.cpp` | Per-file stub extraction |
| `tests/unit/test_cst_lowering.cpp` | CST → AST lowering |
| `tests/unit/test_global_index.cpp` | Index CRUD, qualified lookups |
| `tests/unit/test_lsp_analysis_context.cpp` | Declaration rebuild, per-body analysis |
| `tests/unit/test_lsp_completion.cpp` | Dot, `::`, bare, type completions |
| `tests/unit/test_lsp_hover.cpp` | Hover on vars, functions, fields, types |
| `tests/unit/test_lsp_references.cpp` | Find references, rename, symbol categories |
| `tests/fuzz/fuzz_lsp_parser.cpp` | Coverage-guided libFuzzer target (see `tests/fuzz/README.md`) |
| `tests/unit/test_fuzz_regression.cpp` | Replays the seed corpus + `examples/` through the parser harnesses each test run |

The `roxy_lsp` library depends on `roxy_shared` (lexer, tokens) and `roxy_compiler` (AST types for CST-to-AST lowering).

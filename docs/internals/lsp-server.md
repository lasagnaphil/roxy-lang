# LSP Server

> **Status:** Phases 1–7 implemented (CST, indexing, go-to-definition, completion, hover, semantic diagnostics, find references, rename). Phases 8–9 pending.

This document describes the architecture for the Roxy LSP server, which provides IDE features: diagnostics, completions, hover, go-to-definition, and more.

## Architecture: Map-Reduce

The LSP server uses a **Map-Reduce** architecture (as described in [Three Architectures for a Responsive IDE](https://rust-analyzer.github.io/blog/2020/07/20/three-architectures-for-responsive-ide.html)):

1. **Map (per-file indexing):** Each file is independently parsed and indexed to produce lightweight "stubs" containing top-level declarations with unresolved types.
2. **Reduce (global index):** All stubs are merged into a single index mapping qualified names to types, functions, methods, and traits.
3. **Lazy analysis:** Full semantic analysis runs on-demand per function, using the global index for name resolution.

### Why Map-Reduce

Roxy's design satisfies the prerequisites for map-reduce:

- **Explicit module system** — `import`/`from` gives clear file boundaries
- **No metaprogramming** — no macros or codegen that creates cross-file top-level declarations
- **Self-describing declarations** — `struct Point { x: i32; }`, `fun Point.sum(): i32`, `fun Point.eq() for Eq` all declare their associations syntactically
- **FQN-style resolution** — module-qualified names (`math.sin`) are sufficient for cross-module lookups

The header-based approach doesn't fit (no header files). The query-based incremental approach (salsa/rust-analyzer) is overkill — Roxy has no proc macros, no orphan trait impls, and simple module-to-file mapping.

### Architecture Diagram

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

## Error-Recovering Parser

The compiler parser is fail-fast (stops on first error). The LSP parser must always produce a tree, even for incomplete or malformed code.

### Design: Separate Parser, Shared Lexer

The LSP parser is a **separate implementation** from the compiler parser, sharing only the Lexer and token definitions. This avoids polluting the compiler's fail-fast path with error-recovery complexity.

| Aspect | Compiler Parser | LSP Parser |
|--------|----------------|------------|
| Error handling | Fail-fast (one error) | Error recovery (always produces tree) |
| Tree format | Lossy AST (discards trivia) | Lossless CST (preserves whitespace, comments) |
| Allocation | BumpAllocator | Arena per parse (replaceable) |
| Output | `Program*` (AST nodes) | `SyntaxTree` (CST nodes with NodeId) |

### Concrete Syntax Tree (CST)

The CST preserves all source information. All nodes use a single flat struct (no inheritance):

```cpp
// Byte offset range in source text
struct TextRange { u32 start; u32 end; };

// A single node in the concrete syntax tree
struct SyntaxNode {
    SyntaxKind kind;             // Terminal (token) or non-terminal (grammar production)
    TextRange range;             // Byte offset range in source
    SyntaxNode* parent;
    Span<SyntaxNode*> children;  // Finalized from Vector via alloc_span
    Token token;                 // Set for leaf (terminal) nodes
    const char* error_message;   // Set for Error nodes
};
```

- **Leaf nodes** have `kind` in the terminal range (e.g. `TokenIdentifier`) and `token` set.
- **Interior nodes** have `kind` in the non-terminal range (e.g. `NodeVarDecl`) and children populated.
- **Error nodes** have `kind == SyntaxKind::Error` and `error_message` set.

### Recovery Strategies

The parser uses three recovery strategies depending on context:

**1. Synchronization at statement boundaries:**

When an unexpected token is found inside a statement, skip tokens until a synchronization point (`;`, `}`, or a statement-starting keyword like `if`, `while`, `var`, `fun`):

```cpp
SyntaxNode* parse_statement() {
    // ... try normal parsing ...
    if (error) {
        auto* err = make_error_node("expected statement");
        synchronize_to({TokenKind::Semicolon, TokenKind::RBrace,
                        TokenKind::KW_if, TokenKind::KW_while,
                        TokenKind::KW_var, TokenKind::KW_fun});
        return err;
    }
}
```

**2. Synthetic token insertion for expected tokens:**

When a specific token is expected but not found, insert a synthetic (zero-width) token and continue:

```cpp
Token consume_or_synthetic(TokenKind expected, const char* message) {
    if (check(expected)) { advance(); return m_previous; }
    // Insert synthetic zero-width token at current position
    add_diagnostic(TextRange{m_current.loc.offset, m_current.loc.offset}, message);
    Token synthetic = {};
    synthetic.kind = expected;
    synthetic.loc = m_current.loc;
    return synthetic;
}
```

**3. Bracket-aware skipping:**

When recovering inside brackets, skip to the matching close bracket while tracking nesting depth:

```cpp
void skip_to_closing(TokenKind open, TokenKind close) {
    int depth = 1;
    while (depth > 0 && !at_end()) {
        if (check(open)) depth++;
        if (check(close)) depth--;
        if (depth > 0) advance();
    }
    if (check(close)) advance();
}
```

### CST-to-AST Lowering (Phase 3)

For semantic analysis, the CST will be lowered to the compiler's existing AST format. Error nodes produce `nullptr` or sentinel AST nodes:

```cpp
Expr* lower_expr(SyntaxNode* cst_node) {
    if (cst_node->kind == SyntaxKind::Error) {
        return make_error_expr(cst_node->range);
    }
    switch (cst_node->kind) {
        case SyntaxKind::BinaryExpr:
            return lower_binary_expr(cst_node);
        // ...
    }
}
```

The semantic analyzer already has an `Error` type kind. The LSP extends this so that expressions/statements containing error nodes propagate `Error` types without cascading false diagnostics.

## Per-File Index (Stubs)

The indexer runs after the error-recovering parse and extracts top-level declarations into lightweight stubs. This is cheap (syntax-only, no type resolution) and parallelizable across files.

### Stub Types

```cpp
// A single file's index
struct FileStubs {
    StringView module_name;
    StringView file_path;
    u64 content_hash;                    // For change detection

    Vector<StructStub> structs;
    Vector<EnumStub> enums;
    Vector<FunctionStub> functions;
    Vector<MethodStub> methods;          // fun Type.name()
    Vector<ConstructorStub> constructors;
    Vector<DestructorStub> destructors;
    Vector<TraitStub> traits;
    Vector<TraitImplStub> trait_impls;   // fun Type.name() for Trait
    Vector<ImportStub> imports;
    Vector<GlobalVarStub> globals;
};
```

Each stub captures the declaration's **syntax** without resolving types:

```cpp
struct StructStub {
    StringView name;
    TextRange range;                  // Full declaration range
    TextRange name_range;             // Just the name token
    bool is_pub;
    StringView parent_name;           // Empty if no inheritance
    Vector<FieldStub> fields;
    Vector<WhenClauseStub> when_clauses;
    Vector<TypeParamStub> type_params; // For generics
};

struct FieldStub {
    StringView name;
    TypeExpr* type_expr;              // Unresolved type
    bool is_pub;
    Expr* default_value;              // nullptr if no default
    TextRange range;
};

struct FunctionStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    bool is_native;
    Vector<ParamStub> params;
    TypeExpr* return_type;            // Unresolved, nullptr for void
    Vector<TypeParamStub> type_params;
    bool has_body;                    // false for native declarations
};

struct MethodStub {
    StringView struct_name;
    StringView method_name;
    TextRange range;
    TextRange name_range;
    bool is_pub;
    Vector<ParamStub> params;
    TypeExpr* return_type;
    StringView trait_name;            // For "for Trait" implementations
    Vector<TypeExpr*> trait_type_args; // For "for Trait<Args>"
};

struct TraitStub {
    StringView name;
    TextRange range;
    TextRange name_range;
    StringView parent_name;           // Trait inheritance
    Vector<TypeParamStub> type_params;
    Vector<TraitMethodStub> methods;  // Required + default
};

struct ParamStub {
    StringView name;
    TypeExpr* type_expr;
    ParamModifier modifier;           // None, Out, Inout
};

struct TypeParamStub {
    StringView name;
    Vector<TraitBoundStub> bounds;    // <T: Printable + Hash>
};

struct ImportStub {
    StringView module_path;           // "math" or "math.vec2"
    Vector<StringView> imported_names; // Empty for "import X", filled for "from X import a, b"
    bool is_wildcard;                 // "from X import *"
};
```

### What the Indexer Extracts

The indexer walks the CST (or lowered AST) and extracts:

| Declaration | Extracted Information |
|-------------|---------------------|
| `struct Point { x: i32; y: i32; }` | Name, fields (names + unresolved types), parent, when clauses, generics |
| `enum Color { Red, Green, Blue }` | Name, variants (names + values) |
| `fun add(a: i32, b: i32): i32 { ... }` | Name, params (names + unresolved types), return type, visibility |
| `fun Point.sum(): i32 { ... }` | Struct name, method name, params, return type |
| `fun Point.eq(other: Point): bool for Eq { ... }` | Struct name, method name, trait name, trait type args |
| `fun new Point(x: i32, y: i32) { ... }` | Struct name, constructor name, params |
| `fun delete Point() { ... }` | Struct name, destructor name |
| `trait Printable;` | Trait name, parent trait, type params |
| `import math;` | Module path |
| `from math import sin, cos;` | Module path, imported symbols |
| `var global_count: i32 = 0;` | Name, unresolved type |

The indexer does **not** look inside function bodies. Bodies are analyzed lazily on demand.

## Global Index

The global index merges all per-file stubs into a unified lookup structure.

### Index Structure

```cpp
class GlobalIndex {
    // Module → file mapping
    tsl::robin_map<StringView, FileStubs*> m_module_to_file;

    // Qualified name → declaration lookups
    tsl::robin_map<StringView, StructStub*> m_structs;
    tsl::robin_map<StringView, EnumStub*> m_enums;
    tsl::robin_map<StringView, TraitStub*> m_traits;
    tsl::robin_map<StringView, FunctionStub*> m_functions;

    // Struct → associated declarations
    tsl::robin_map<StringView, Vector<MethodStub*>> m_methods_by_struct;
    tsl::robin_map<StringView, Vector<ConstructorStub*>> m_constructors_by_struct;
    tsl::robin_map<StringView, Vector<DestructorStub*>> m_destructors_by_struct;
    tsl::robin_map<StringView, Vector<TraitImplStub*>> m_trait_impls_by_struct;

public:
    // Update index for a single file (on edit)
    void update_file(FileStubs* stubs);

    // Remove a file from the index (on delete)
    void remove_file(StringView file_path);

    // Lookups
    StructStub* find_struct(StringView name);
    EnumStub* find_enum(StringView name);
    TraitStub* find_trait(StringView name);
    FunctionStub* find_function(StringView name);
    Span<MethodStub*> find_methods(StringView struct_name);
    Span<ConstructorStub*> find_constructors(StringView struct_name);

    // Cross-cutting queries
    Vector<MethodStub*> find_trait_implementations(StringView trait_name);
    Vector<StructStub*> find_subtypes(StringView struct_name);
};
```

### Index Update on File Edit

When a file is edited:

1. Re-lex and re-parse the file (error-recovering parser)
2. Re-run the indexer to produce new `FileStubs`
3. Compare the new stubs against the old ones (by content hash per declaration)
4. If top-level declarations changed: update the global index, invalidate dependent caches
5. If only function bodies changed: no index update needed (bodies aren't indexed)

This is the key insight from the map-reduce architecture: **most edits are inside function bodies and don't affect the index**. Only structural changes (adding/removing/renaming declarations, changing field types) trigger index updates.

## Lazy Semantic Analysis

Full type checking runs on demand, not eagerly for the whole project. When the user requests completions or hovers inside a function body, the analyzer processes that function against the global index.

### Analysis Scope

```cpp
class LazyAnalyzer {
    GlobalIndex& m_index;
    TypeEnv m_type_env;          // Shared type environment
    BumpAllocator m_alloc;

public:
    // Analyze a single function body for diagnostics, types, etc.
    AnalysisResult analyze_function(FunctionStub* func, FileStubs* file);

    // Resolve a type expression against the index
    Type* resolve_type(TypeExpr* expr, FileStubs* file);

    // Get completions at a position inside a function body
    Vector<CompletionItem> completions_at(FileStubs* file, Position pos);

    // Get the type of an expression at a position
    Type* type_at(FileStubs* file, Position pos);

    // Find the definition of a symbol at a position
    Location definition_of(FileStubs* file, Position pos);
};
```

### Analysis Phases (Lazy)

The lazy analyzer mirrors the compiler's passes but operates on a subset:

**Phase 1 — Resolve types from index (cached globally):**

On first use (or after index invalidation), resolve all struct/enum/trait stubs into `Type*` entries in `TypeEnv`. This populates the TypeCache with struct fields, enum variants, method signatures, and trait definitions. This is equivalent to compiler passes 1 through 2, but driven from stubs rather than AST.

This phase only re-runs when the global index changes (top-level edit). For body-only edits, the resolved types are reused.

**Phase 2 — Analyze function body (per-request):**

When the user needs information about a specific function (diagnostics, completions, hover):

1. Parse the function body (from the CST or re-parse the body region)
2. Lower to AST
3. Run the compiler's expression/statement analysis logic (type checking, inference, symbol resolution) in a temporary scope
4. Collect diagnostics, resolved types, and symbol references

This is equivalent to compiler pass 3 but scoped to a single function.

### Caching Strategy

```
                    ┌─────────────────────────────────┐
                    │         Cache Layers             │
                    │                                  │
  Index change ────>│  Layer 1: Resolved Types         │ (invalidated on index change)
                    │    TypeEnv with all struct/enum   │
                    │    types, methods, traits         │
                    │                                  │
  Body edit ───────>│  Layer 2: Per-Function Analysis   │ (invalidated on body edit)
                    │    Diagnostics, resolved exprs,  │
                    │    local symbol table             │
                    └─────────────────────────────────┘
```

- **Layer 1** is rebuilt when the global index changes. Since it only depends on top-level declarations, it's stable across most edits.
- **Layer 2** is rebuilt per-function when the function body is edited. Since functions are typically small, this is fast.

## LSP Features

### Diagnostics (`textDocument/publishDiagnostics`)

**Strategy:** Two-tier diagnostics.

1. **Syntax diagnostics (immediate):** Reported from the error-recovering parser on every keystroke. These are cheap (lexer + parser only, no semantic analysis).
2. **Semantic diagnostics (debounced):** Run lazy analysis on the current file after a short debounce (e.g., 200ms of inactivity). Report type errors, unresolved symbols, trait bound violations, etc.

Cross-file semantic diagnostics (e.g., missing import, type mismatch in cross-module call) are reported when the file's imports are resolved against the global index.

### Completions (`textDocument/completion`)

**Trigger:** User types `.` or `::` or a partial identifier.

**Strategy:**

1. Determine the expression to the left of the cursor from the CST
2. If it's a `.` access on a typed expression:
   - Resolve the receiver's type (may require lazy analysis of the partial function body up to the cursor)
   - Use `TypeCache::lookup_method()` hierarchy to enumerate methods
   - Include fields from `StructTypeInfo.fields`
   - Include inherited methods from parent structs
   - Include trait methods from `implemented_traits`
3. If it's a `::` access on a type name:
   - Look up the type in the global index
   - List enum variants (for enum types)
4. If it's a bare identifier:
   - Walk the scope chain for local variables and parameters
   - Search the global index for functions, structs, enums, traits
   - Search imported module exports
5. If it's a type annotation position (after `:`):
   - List all known types (primitives, structs, enums, generic types)
   - Include `uniq`, `ref`, `weak` modifiers

### Hover (`textDocument/hover`)

**Strategy:**

1. Find the CST node at the cursor position
2. If it's an identifier in an expression: run lazy analysis to determine the expression's resolved type, display the type signature
3. If it's a type name: look up in the global index, display the type definition
4. If it's a method call: resolve the method via `TypeCache::lookup_method()`, display the full signature including the struct it belongs to and any trait it implements
5. If it's a keyword: display keyword documentation

### Go-to-Definition (`textDocument/definition`)

**Strategy:**

1. Find the symbol at the cursor position (identifier, type name, method name)
2. For local variables/parameters: the definition is within the current function body (from lazy analysis scope)
3. For functions: look up in the global index, return the `FunctionStub.name_range`
4. For types (struct/enum/trait): look up in the global index, return the `name_range`
5. For methods: look up in `m_methods_by_struct`, return the stub's range
6. For fields: look up the struct stub, find the field, return its range
7. For imported symbols: follow the import to the source module, find the export's declaration

### Find References (`textDocument/references`)

**Strategy:**

This is the most expensive operation. Two approaches:

1. **Lazy (good enough for small projects):** Search all file CSTs for identifier tokens matching the target name, then filter by running lazy analysis to confirm they refer to the same symbol.
2. **Indexed (better for larger projects):** During per-file indexing, also record references to external symbols. Store a reverse index: symbol → list of (file, range) pairs.

Start with approach 1 (simpler). Move to approach 2 if performance is insufficient.

### Document Symbols (`textDocument/documentSymbol`)

**Strategy:** Directly from the per-file stubs. List all top-level declarations with their names, kinds, and ranges. This requires no semantic analysis.

### Rename (`textDocument/rename`)

**Strategy:**

1. Identify the symbol at the cursor (via go-to-definition logic)
2. Find all references (via find-references logic)
3. Compute text edits for all locations
4. For method/constructor renaming, also update the mangled name references

## Threading Model

```
                    ┌─────────────────────────────────┐
                    │         Main Thread              │
                    │  (LSP message dispatch)          │
                    │                                  │
                    │  didOpen/didChange ──> Queue     │
                    │  completion/hover  ──> Queue     │
                    └────────────┬────────────────────┘
                                 │
                    ┌────────────▼────────────────────┐
                    │      Analysis Thread Pool        │
                    │                                  │
                    │  Worker 1: Re-index file A       │
                    │  Worker 2: Analyze function body  │
                    │  Worker 3: Re-index file B       │
                    └──────────────────────────────────┘
```

- **Main thread:** Receives LSP messages, dispatches to workers, sends responses
- **Worker threads:** Run indexing (parallelizable per-file) and lazy analysis
- **Cancellation:** When a new edit arrives, cancel in-progress analysis for that file and restart. Use a generation counter to discard stale results.

### Request Prioritization

| Priority | Request Type | Reason |
|----------|-------------|--------|
| High | `completion`, `signatureHelp` | User is actively typing |
| Medium | `hover`, `definition` | User is reading/navigating |
| Low | `references`, `rename`, `diagnostics` | Background or explicit user action |

High-priority requests preempt low-priority background work.

## Interaction with Existing Code

### Reused Components

| Component | Reuse Strategy |
|-----------|---------------|
| **Lexer** (`shared/lexer.hpp`) | Shared as-is. Already handles all token types, positions, f-strings. |
| **Token kinds** (`shared/token_kinds.hpp`) | Shared as-is. Single source of truth for keywords and operators. |
| **TypeCache** (`compiler/types.hpp`) | Shared as-is. Type interning, method lookup, trait checking all reusable. |
| **TypeEnv** (`compiler/type_env.hpp`) | Shared with modifications. Need invalidation support for index updates. |
| **ModuleRegistry** (`compiler/module_registry.hpp`) | Shared as-is. Manages native + script module lookups. |
| **NativeRegistry** (`vm/binding/registry.hpp`) | Shared as-is. Built-in function signatures needed for completions. |
| **GenericInstantiator** (`compiler/generics.hpp`) | Shared with care. Template registration reusable; instantiation may need per-request arenas. |

### New Components

| Component | Purpose |
|-----------|---------|
| **LSP Parser** (`lsp/lsp_parser.hpp`) | Error-recovering parser producing CST |
| **CST nodes** (`lsp/syntax_tree.hpp`) | Lossless syntax tree with NodeId for incremental updates |
| **File Indexer** (`lsp/indexer.hpp`) | Extracts stubs from CST |
| **Global Index** (`lsp/global_index.hpp`) | Merges stubs, provides qualified lookups, type info, field defaults |
| **CST Lowering** (`lsp/cst_lowering.hpp`) | Lowers CST to compiler AST for function bodies |
| **LspTypeResolver** (`lsp/lsp_type_resolver.hpp`) | Lazy per-function type resolution, variable scoping, semantic diagnostics |
| **LSP Transport** (`lsp/transport.hpp`) | JSON-RPC over stdin/stdout |
| **LSP Server** (`lsp/server.hpp`) | Request dispatch, state management |

## File Structure

```
include/roxy/lsp/
    syntax_tree.hpp          # CST node types, SyntaxKind, TextRange
    lsp_parser.hpp           # Error-recovering parser
    indexer.hpp              # Per-file stub extraction
    global_index.hpp         # Merged index with qualified lookups
    cst_lowering.hpp         # CST-to-AST lowering for function bodies
    lsp_type_resolver.hpp    # Lazy per-function type resolution + semantic diagnostics
    transport.hpp            # JSON-RPC stdin/stdout transport
    server.hpp               # LSP server main class
    protocol.hpp             # LSP protocol types (Position, Range, etc.)

src/roxy/lsp/
    lsp_parser.cpp
    indexer.cpp
    global_index.cpp
    cst_lowering.cpp
    lsp_type_resolver.cpp
    transport.cpp
    server.cpp

tests/unit/
    test_lsp_parser.cpp              # 15 cases: CST parsing, error recovery
    test_global_index.cpp            # 21 cases: index CRUD, qualified lookups
    test_lsp_type_resolver.cpp       # 15 cases: variable scope, type resolution
    test_lsp_completion.cpp          # 16 cases: dot, ::, bare, type completions
    test_lsp_hover.cpp               # 14 cases: hover on vars, functions, fields, types
    test_lsp_semantic_diagnostics.cpp # 31 cases: unresolved symbols, type mismatches, missing fields
    test_lsp_references.cpp          # 15 cases: find references, rename, symbol categories
```

CMake adds a `roxy_lsp` library depending on `roxy_shared` (lexer, tokens) and `roxy_compiler` (AST types for CST-to-AST lowering).

## Implementation Phases

### Phase 1: CST Infrastructure + Diagnostics ✓

**Goal:** Open a file, see syntax errors in the editor.

- [x] Define `SyntaxKind` enum covering all grammar productions and tokens
- [x] Implement flat `SyntaxNode` CST type (terminals, non-terminals, error nodes)
- [x] Implement error-recovering parser with synchronization, synthetic tokens, and bracket-aware skipping
- [x] Implement LSP transport (JSON-RPC over stdin/stdout)
- [x] Implement `textDocument/didOpen`, `textDocument/didChange`, `textDocument/didClose` handlers
- [x] Publish syntax diagnostics on file open/change
- [x] Test: 15 test cases covering clean source, error recovery, CST structure, and source ranges

**Files:** `syntax_tree.hpp`, `lsp_parser.hpp/.cpp`, `transport.hpp/.cpp`, `server.hpp/.cpp`, `protocol.hpp`
**Tests:** `test_lsp_parser.cpp` (15 cases)

### Phase 2: Per-File Indexing + Document Symbols ✓

**Goal:** See document outline, navigate by symbol.

- [x] Implement stub types (`StructStub`, `FunctionStub`, `MethodStub`, `ConstructorStub`, `DestructorStub`, `TraitStub`, `EnumStub`, `GlobalVarStub`, etc.)
- [x] Implement file indexer (CST → stubs)
- [x] Implement `textDocument/documentSymbol` from stubs
- [x] Test: document symbols list shows structs, functions, methods

**Files:** `indexer.hpp/.cpp`
**Tests:** `test_global_index.cpp` (21 cases, also covers Phase 3)

### Phase 3: Global Index + Go-to-Definition ✓

**Goal:** Navigate across files with go-to-definition.

Split into two sub-phases:

**Phase 3a:** Global index and identifier-based go-to-definition
- [x] Implement `GlobalIndex` with qualified name lookups (structs, enums, functions, traits, globals, methods, constructors, fields)
- [x] Build index on workspace open (index all `.roxy` files)
- [x] Implement incremental index update on file change (remove old entries, re-index)
- [x] Implement `textDocument/definition` for types, functions, global variables

**Phase 3b:** CST-to-AST lowering and field-access go-to-definition
- [x] Implement CST-to-AST lowering for function bodies (`CstLowering`)
- [x] Implement `LspTypeResolver` for lazy per-function type resolution using `GlobalIndex`
- [x] Implement go-to-definition for field access (`obj.field`), method calls, `self`, `self.field`
- [x] Type information maps in `GlobalIndex`: struct parents, field types, function/method return types

**Files:** `global_index.hpp/.cpp`, `cst_lowering.hpp/.cpp`, `lsp_type_resolver.hpp/.cpp`
**Tests:** `test_global_index.cpp` (21 cases), `test_lsp_type_resolver.cpp` (15 cases)

### Phase 4: Completions ✓

**Goal:** Dot-completion for struct fields and methods.

- [x] Implement lazy function body analysis (LspTypeResolver builds variable scope from AST)
- [x] Implement `.` completions: fields and methods (including inherited via hierarchy walk)
- [x] Implement `::` completions: enum variants
- [x] Implement bare identifier completions: locals, globals, functions, structs, enums, traits
- [x] Implement type annotation completions: primitives, structs, enums
- [x] Completion detail strings with signatures and type info

**Files:** `server.cpp` (completion handler), `global_index.hpp/.cpp` (completion indexes, signatures)
**Tests:** `test_lsp_completion.cpp` (16 cases)

### Phase 5: Hover ✓

**Goal:** Hover for type info.

- [x] Implement `textDocument/hover` with type signatures
- [x] Hover on variables (show type), functions (show signature), struct fields, methods, types, enum variants, globals
- [x] CST-based expression type resolution (`resolve_cst_expr_type`) for hover on chained expressions

**Files:** `server.cpp` (hover handler), `lsp_type_resolver.hpp/.cpp` (CST expression resolver)
**Tests:** `test_lsp_hover.cpp` (14 cases)

### Phase 5b: Semantic Diagnostics ✓

**Goal:** Semantic error reporting beyond syntax errors.

- [x] Unresolved identifiers, functions, types
- [x] Unknown type annotations
- [x] Unresolved field access on known struct types
- [x] Unresolved method calls on known struct types
- [x] Unresolved enum variants
- [x] Wrong argument count for functions, methods, constructors
- [x] Struct literal field validation (unknown field on struct)
- [x] Cascade prevention (unknown type doesn't cascade to field/method errors)
- [x] Param count checking through inheritance hierarchy

**Files:** `lsp_type_resolver.hpp/.cpp` (diagnostic collection), `server.cpp` (publishing)
**Tests:** `test_lsp_semantic_diagnostics.cpp` (14 cases in Phase 5b)

### Phase 6: Expanded Semantic Diagnostics ✓

**Goal:** Additional lightweight diagnostics using string-based type comparison.

- [x] **Literal type resolution:** `ExprLiteral` and `ExprStringInterp` resolve to type names (i32, f64, string, bool, nil, etc.)
- [x] **Duplicate parameter names:** O(n²) scan across all function/method/constructor/destructor declarations
- [x] **Var type mismatch:** `var x: i32 = "hello"` detects annotation vs initializer type mismatch
- [x] **Return type mismatch:** `return "hello"` in `fun f(): i32` detects return value vs declared return type mismatch
- [x] **Missing required fields:** `Point { x = 1.0 }` reports missing field `y` when it has no default; fields with defaults are skipped
- [x] **Named constructor validation:** `Point.from_polar(1.0, 2.0)` checks constructor existence and argument count
- [x] **Type mismatch safety:** Skip empty types (cascade prevention), nil (allowed for references), parameterized types (contains `<`), numeric-to-numeric (avoids false positives like `var x: i64 = 42`)
- [x] **GlobalIndex field default tracking:** `field_has_default()` query backed by `m_field_has_defaults` map

**Files:** `lsp_type_resolver.hpp/.cpp`, `global_index.hpp/.cpp`
**Tests:** `test_lsp_semantic_diagnostics.cpp` (17 new cases, 31 total)

### Phase 7: Find References + Rename ✓

**Goal:** Find all usages, rename symbols.

- [x] Implement `textDocument/references` (lazy approach: search all file CSTs for matching identifiers, filter by semantic context)
- [x] Symbol identity system (`SymbolCategory` enum + `SymbolIdentity` struct) for canonical symbol identification
- [x] Reference filtering: context-aware checking per symbol category (function, struct, enum, trait, global, method, field, local, parameter)
- [x] Method/field disambiguation: resolve receiver type via `LspTypeResolver` to match references to the correct struct
- [x] Local/parameter scoping: only search within the enclosing function in the same file
- [x] `includeDeclaration` support: optionally exclude definition from results
- [x] Implement `textDocument/rename` with cross-file `WorkspaceEdit` generation
- [x] Test: 15 cases covering functions, types, fields/methods, locals/params, and edge cases

**Files:** `server.hpp` (handler declarations), `server.cpp` (implementation)
**Tests:** `test_lsp_references.cpp` (15 cases)

### Phase 8: Full Semantic Analysis

**Goal:** Hook up the compiler's `Semantic` pass for precise type resolution.

- [ ] Use TypeCache/TypeEnv for full type inference (generic type inference, trait method dispatch)
- [ ] Cross-type inference chains, precise type mismatch diagnostics
- [ ] Completions after complex expressions (chained method calls, generic instantiations)
- [ ] Replace lightweight string-based LspTypeResolver with proper type-aware analyzer

### Phase 9: Polish

- [ ] Signature help (`textDocument/signatureHelp`) for function calls
- [ ] Code actions (quick fixes for common errors)
- [ ] Workspace symbols (`workspace/symbol`) for project-wide search
- [ ] Semantic token highlighting (`textDocument/semanticTokens`)
- [ ] Threading and cancellation support
- [ ] Performance profiling and optimization

## Dependencies

The LSP server requires:

1. **Existing compiler components** — Lexer, TypeCache, TypeEnv, ModuleRegistry (already implemented)
2. **JSON library** — For LSP JSON-RPC protocol (e.g., vendored nlohmann/json or a minimal parser)
3. **No external LSP library** — The protocol is simple enough to implement directly (JSON-RPC over stdin/stdout)

## Files

| File | Purpose |
|------|---------|
| `include/roxy/lsp/syntax_tree.hpp` | CST node types, SyntaxKind, TextRange |
| `include/roxy/lsp/lsp_parser.hpp` | Error-recovering parser |
| `include/roxy/lsp/indexer.hpp` | Per-file stub extraction |
| `include/roxy/lsp/global_index.hpp` | Merged index with type info, field defaults, param counts |
| `include/roxy/lsp/cst_lowering.hpp` | CST-to-AST lowering |
| `include/roxy/lsp/lsp_type_resolver.hpp` | Lazy type resolution + semantic diagnostics |
| `include/roxy/lsp/transport.hpp` | JSON-RPC transport |
| `include/roxy/lsp/server.hpp` | LSP server (definition, completion, hover, diagnostics, references, rename) |
| `include/roxy/lsp/protocol.hpp` | LSP protocol types |
| `src/roxy/lsp/*.cpp` | Implementations |
| `docs/internals/lsp-server.md` | This document |
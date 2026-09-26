# Frontend Architecture

The Roxy frontend consists of a compiler and an LSP server that share the lexer and token kinds. See [lsp-server.md](lsp-server.md) for the LSP architecture.

```
Source → Lexer → Parser → AST → Semantic Analysis → IR Builder → SSA IR → Lowering → Bytecode → VM
```

## Compiler vs. LSP

| Compiler | LSP |
|----------|-----|
| Fail-fast parser | Error-recovering parser |
| AST (lossy) | CST (lossless, preserves trivia) |
| Batch processing | Per-file indexing + lazy per-function analysis |
| One arena per compile | One arena per parse (re-parsed per keystroke) |

The LSP side reuses the shared lexer, token kinds, AST, and — through `LspAnalysisContext` — the semantic analyzer itself, but runs its own error-recovering parser over a lossless CST.

## Key Design Decisions

| Stage | Approach | Rationale |
|-------|----------|-----------|
| Parsing | Separate compiler/LSP parsers, shared lexer | Compiler can fail-fast; LSP needs error recovery |
| IR | SSA with block arguments (not phi nodes) | Cleaner dataflow, easier lowering |
| Bytecode | Register-based, 32-bit fixed-width | Easy C transpilation, natural SSA lowering |
| Memory | Bump/arena allocation throughout | Fast batch compile; whole-arena reset per LSP re-parse |

The compiler parser is recursive descent with Pratt parsing for expressions, fail-fast (stops on the first error). Literal syntax (number bases/suffixes, escapes) is specified in `docs/grammar.md`.

## Semantic Analysis

Multi-pass; `analyze()` in `semantic.cpp` is the running order: imports and the builtin prelude, then type declarations and builtin registrations, then type members and signatures, then function bodies. Types are registered before members are resolved, so declaration order doesn't matter.

### All-paths-return

A non-void function or lambda whose body can fall off the end without a `return`/`throw` is rejected ("not all code paths return a value" / "…in lambda"). Termination comes from `LifetimeChecker::branch_terminates()`, which treats as terminating:

- an exhaustive no-`else` `when` whose arms all terminate;
- an infinite loop — but only a literal `while (true)` with no `break` reaching it (`while (1 == 1)` does not count).

Coroutines are skipped (they `yield`, never return a value). A `=> expr` lambda desugars to `{ return expr; }`, so only block-bodied lambdas can be flagged.

### Collaborators

The analyzer's collaborators — `ErrorReporter`, `TypeChecker`, `LifetimeChecker`, `TraitSystem`, `LambdaLifter`, `GenericCallResolver` (responsibilities summarized in `CLAUDE.md`) — share state through the `SemaContext` bundle and hold **no reference to the analyzer**. The analyzer operations they need (full TypeExpr resolution, walker re-entry for generic inference and Phase B bodies) are exposed as function-pointer thunks on the context (`SemaContext::resolve_type_expr` / `analyze_expr` / `analyze_stmt`).

### Per-function state is pushed as one unit

All per-function analysis state — the `FunctionContext` slots (coroutine / delete-destructor / finally depth) and the `LifetimeChecker`'s move states and branch-termination flag — is pushed and popped together by `FunctionContextScope` at every body-analysis entry point (free function, member body, synthesized lambda call function, Phase B template body). A nested body (a lambda analyzed mid-statement) gets a fresh default context: its `return` cannot read as "the enclosing branch terminates", and its `throw` is not "inside" the enclosing delete destructor. The single guard exists because forgetting one slot at one entry point produced repeated bugs.

### Sema → IR contract

Analysis results flow to the IR builder as in-place AST annotations (`resolved_type` plus name/flag fields, including several deliberate overloads such as the callee-type dispatch signal and the null-object module sentinel). The authoritative spec is the "semantic→IR annotation contract" comment above `struct Expr` in `compiler/parse/ast.hpp` — keep it updated when adding or overloading an annotation.

Because analysis also *rewrites* the tree it walks (capture rewrites, generic TypeExpr mangling, lambda synthesis), it is non-idempotent, and the codified rule is: **an AST body is analyzed at most once** (`Decl::body_analyzed` + assert at the body-analysis entry points). Consumers that need to analyze the "same" code twice analyze two trees: the LSP lowers a fresh AST from the CST per analysis, generic instantiations deep-clone the pristine template, Phase B walks a throwaway identity-substitution clone, and trait default methods are cloned per implementing struct. The full rationale lives in the ast.hpp contract block ("the single-shot analysis rule").

# Error Handling (Compiler Internals)

How the Roxy **compiler** signals and propagates failures internally, in C++.
This is distinct from the Roxy **language's** exception feature (`try`/`catch`/
`throw` in user programs) — for that, see `exceptions.md`.

The guiding principle: **the error-handling strategy follows the consumer, not
the operation.** A batch compile of a whole program wants *every* error at once;
an interactive editor wants maximal recovery; a structural invariant check wants
to halt on the first violation; a leaf I/O call just needs success/failure. Each
of these gets a different mechanism, on purpose. There is no single `Result`/
`Error` type threaded through the compiler, and that is a deliberate choice
(see [Why not `Result<T, Error>`](#why-not-resultt-error)).

---

## The two strategies

### 1. Error-collecting (accumulate + never-null sentinels)

Used by the passes that process a whole program and want to report as many real
problems as possible in one run: **semantic analysis** and the **compiler
pipeline**.

**Accumulation.** Diagnostics are pushed into an `ErrorReporter`
(`compiler/error_reporter.hpp`), not returned. Each `SemanticError` carries a
`SourceLocation` and a message (owned in the bump allocator when formatted via
`error_fmt`). Collection is capped so a pathological input doesn't spew
unboundedly — `MAX_SEMANTIC_ERRORS` (20) in batch mode, `MAX_LSP_SEMANTIC_ERRORS`
(200) in LSP mode — via `too_many_errors()`, which every reporting entry point
checks first.

**Never-null sentinels.** So a *local* failure doesn't force every downstream
reader to null-check, analysis substitutes a sentinel and keeps going.
`SemanticAnalyzer::resolve_type_expr` (`semantic.hpp`) **never returns null**: a
null `TypeExpr` (from an LSP-recovered AST) and every resolution failure both
yield `error_type`, with the error reported at the failure site. Callers that
need to distinguish a *missing* annotation (e.g. `var`-decl inference) branch on
the `TypeExpr` itself, not on the resolved `Type*`. The `error_type` value then
flows through the rest of analysis inertly — comparisons against it succeed,
coercions short-circuit — so one bad annotation produces one diagnostic, not a
cascade.

**Pipeline plumbing.** The phase methods (`parse_all`, `analyze_all`,
`build_ir_all`, `topological_sort`) return `bool` for "did this phase succeed",
while the *details* live in the side-channel error list. `Compiler::compile()`
(`compiler.hpp`) returns `BCModule*` and yields `nullptr` on failure; callers
inspect `has_errors()` / `errors()` for the messages. The `bool` is a gate, not
the payload.

### 2. Fail-fast (bool / null sentinel, halt on first)

Used where continuing past a failure buys nothing — either because the output is
already meaningless, or because a failure means a *compiler bug* rather than a
user error.

- **Strict parser** (`compiler/parser.hpp`) — recursive descent with a single
  `m_has_error` flag (`has_error()`), reporting through `report_error_at`. Node
  constructors return `Expr*`/`Stmt*` and yield null on failure. This parser does
  **not** recover; the error-recovering variant lives in the LSP (see below).
- **IR validator** (`compiler/ir_validator.hpp`) — a structural-integrity check
  between IR building and lowering. Malformed IR is a compiler bug, so it stops
  at the first violation: `m_has_error` + a single `m_error`/`m_error_buf`,
  surfaced via `has_error()` / `error()`.
- **Leaf I/O and parse ops** — `read_file_to_buf` (`core/file.hpp`) returns
  `bool` with the buffer in an out-parameter; `json_parse` / `JsonParser::parse`
  (`core/json.hpp`) return `bool` and drive a SAX-style `Handler` whose callbacks
  themselves return `bool` (false = stop). The outcome is binary and the caller
  decides what to do with it.

---

## The LSP exception: error recovery

The LSP is the one consumer that wants the *opposite* of fail-fast. Its parser
(`lsp/lsp_parser.{hpp,cpp}`) is error-recovering and produces a lossless CST
using three strategies — synthetic token insertion, statement-boundary
synchronization, and bracket-aware skipping — so a half-typed buffer still yields
a usable tree and diagnostics. Semantic analysis in LSP mode reuses strategy 1
(accumulate + `error_type`), just with the higher error cap and null-tolerant
walkers, because the never-null sentinel contract is exactly what lets analysis
survive a partially-broken AST. (The precise per-pass null-tolerance *policy* for
LSP-recovered ASTs is still being nailed down — see the TODO on defining it.)

---

## Why not `Result<T, Error>`

A monadic `Result<T, Error>` (short-circuit-on-first-error, `?`-style
propagation) was considered and **deliberately not adopted**. It is the wrong
shape for this compiler:

- **It fights error collection.** `Result` propagates the *first* error and
  stops. The dominant passes (semantic, pipeline) are built to *accumulate* and
  keep going so one compile surfaces many diagnostics. Wrapping them in `Result`
  would regress multi-error reporting and the LSP's recovery.
- **It undoes the sentinel design.** The never-null `error_type` return exists
  precisely so downstream analysis *doesn't* branch on every resolution.
  `Result<Type*, E>` would reintroduce an `is_err()` check at every one of those
  sites — the exact noise the sentinel removes — for no gain.
- **The fail-fast leaves don't need it.** File I/O, JSON, the parser, and the IR
  validator have binary outcomes whose callers rarely use structured error
  detail; they print a message or halt. `bool` + an out-param (or a documented
  error field) is locally clear, and `Result` would be ceremony without payoff.

The net: the existing split is coherent and matched to each consumer. A
project-wide `Result` type would add churn in the fail-fast leaves and cause
active harm in the error-collecting passes.

---

## Diagnostic message style

The two strategies above decide *how* a failure propagates; this section fixes
*how the message reads*, so diagnostics from the lexer, parser, and semantic
passes look like one voice. User-facing messages print after a location prefix
(`… in module 'm' at line 4: <message>`), which shapes the rules.

**Case and punctuation.** A user-facing diagnostic **starts lowercase and has no
trailing period** — `undefined identifier 'x'`, not `Undefined identifier 'x'.`
This is the Rust/Clang/Swift convention and reads correctly after the `at line N:`
colon. The first word is lowercased *unless* it is a quoted keyword or a
type/trait name that carries its own capital (`List constructor …`, `'Self' can
only be used …`, `Coro requires …`). A multi-sentence diagnostic (a message plus
an inline hint) still starts lowercase and drops the *trailing* period, but its
internal sentences keep their capitals and punctuation — e.g. `conditional
expression cannot select a noncopyable value; it would move an operand without
nullifying it (double-free). Use an if/else statement … instead`.

**Quoting.** Wrap code fragments — keywords, identifiers, and rendered types — in
single quotes: `'when'`, `'ref'`, `'uniq {}'`, `struct '{}' has no field '{}'`.
Keywords are always quoted (`'when' discriminant …`, never `when discriminant …`).

**Canonical phrasings** for recurring families, so the same failure reads the same
way everywhere:

- **Arity mismatch:** `<subject> expects <N> argument(s) but got <M>` (or `<N> to
  <M>` for an optional-argument range). The verb is always *expects* (never
  *takes*); the count noun is the literal `argument(s)` (no hand-rolled
  singular/plural); the tail is always `but got`. `<subject>` names what was
  called — `function '{}'`, `method '{}'`, `constructor`, `destructor`, `parent
  constructor`, `List constructor`, or a bare `call` when no name is in hand. The
  type-argument variant inserts `type`: `… expects <N> type argument(s) but got
  <M>`. Fixed-arity builtins use a distinct, deliberately separate form —
  `List requires exactly 1 type argument`, `Map requires exactly 2 type
  arguments` — because the count is a constant, not a mismatch to report.
- **Trait conformance:** always append the word *trait* —
  `… does not implement the Exception trait`, `catch type must be a struct type
  that implements the Exception trait`.

**Scope.** These rules govern *user-facing* diagnostics: the lexer
(`error_token`), the strict and LSP parsers (keep their shared messages in step),
and the semantic analyzer and its collaborators (`ErrorReporter::error` /
`error_fmt`). Internal invariant diagnostics — the IR validator and the lowering
`Internal error: …` messages — are compiler-bug reports, not things a Roxy author
sees, and stay in their own format (`function '{}' block {}: …`); they are exempt.

---

## Adding a new fallible operation

Pick the strategy by consumer, matching what's already there:

1. **Inside an error-collecting pass** (semantic / pipeline): report via the
   `ErrorReporter` (`error` / `error_fmt`) and return an inert sentinel the
   caller tolerates — `error_type` for a `Type*`, or a null AST node that
   downstream walkers already null-check. Do **not** halt the pass. Word the
   message per [Diagnostic message style](#diagnostic-message-style).
2. **A structural invariant** ("can only fail if the compiler is buggy"):
   fail-fast with a clear message, IR-validator style — first error wins, stop.
3. **A leaf I/O / parse op** with a binary outcome: return `bool` (with the
   value in an out-param), or set a documented error field the caller reads.
4. **Do not** introduce a `Result<T, Error>` monad — it works against the
   accumulate-and-continue architecture above.

---

## See also

- `frontend.md` — lexer, parser, semantic analysis passes
- `exceptions.md` — the Roxy *language's* exception feature (a runtime construct,
  not compiler error handling)
- `modules.md` — pipeline phases and multi-module linking
- `lsp-server.md` — the error-recovering parser and LSP diagnostics

# Roxy for Zed

Syntax highlighting (Tree-sitter) and LSP support (`roxy_lsp_server`) for Zed.

Unlike `editors/vscode` and `editors/jetbrains`, this **cannot** reuse
`roxy.tmLanguage.json` — Zed has no TextMate support and highlights exclusively
from Tree-sitter. So this directory carries its own parser.

```
extension.toml          # manifest: grammar + language server
Cargo.toml, src/lib.rs  # WASM extension; launches roxy_lsp_server
languages/roxy/         # config.toml + highlight/bracket/indent/outline queries
tree-sitter-roxy/       # the grammar itself
```

## Installing

The grammar is fetched by git, so **it must be committed before Zed can load
it** — Zed runs `git init` + `git remote add` + fetch at `rev` (see
`compile_grammar` in Zed's `extension_builder.rs`), even for a dev extension.

1. Commit this directory.
2. Point `[grammars.roxy]` in `extension.toml` at a commit that contains it.
   While iterating locally, use your checkout:

   ```toml
   [grammars.roxy]
   repository = "file:///absolute/path/to/roxy"
   rev = "<local commit sha>"
   path = "editors/zed/tree-sitter-roxy"
   ```

   Once pushed, switch `repository` back to the GitHub URL and pin the pushed sha.
3. Build the language server: `ninja -C build roxy_lsp_server`.
4. In Zed: `zed: install dev extension` and choose this directory.

Every grammar change needs `tree-sitter generate`, a commit, and a `rev` bump
before Zed sees it.

### Language server discovery

`roxy_lsp_server` is found on `PATH`, matching the JetBrains plugin. To point at
it explicitly, in `settings.json`:

```json
{
  "lsp": {
    "roxy-lsp": { "binary": { "path": "/abs/path/to/build/roxy_lsp_server" } }
  }
}
```

## Working on the grammar

```bash
cd tree-sitter-roxy
tree-sitter generate          # regenerates src/parser.c
tree-sitter parse ../../../examples/showcase.roxy
```

`src/parser.c`, `src/grammar.json` and `src/node-types.json` are generated but
are **committed on purpose**: Zed compiles `src/parser.c` to WASM with clang and
never invokes `tree-sitter generate` itself.

`src/scanner.c` is hand-written. It exists because two things can't be expressed
in `grammar.js`, both mirroring `lexer.cpp`:

- **nested block comments** (`/* a /* b */ c */`)
- **f-string segmentation** — `FStringBegin` (`f"..{`), `FStringMid` (`}..{`),
  `FStringEnd` (`}.."`), including the degenerate case where an
  interpolation-free `f"..."` collapses to a plain string literal
  (`lexer.cpp:475`).

### The parser is the ground truth

`grammar.js` is modelled on `src/roxy/compiler/parser.cpp`, not on
`docs/grammar.md` — the doc has drifted:

| `docs/grammar.md` says | the parser actually does |
|---|---|
| no bitwise/shift levels in the precedence chain | `\|`(3) `^`(4) `&`(5) bind **looser** than `==`(6); `<<`/`>>`(8) sit between comparison and `+`/`-` |
| `import_list -> "*" \| ...` | no `*`; an identifier list with optional `as` aliases |
| `destructor_decl -> "fun" "delete" Ident "(" ")"` | destructors take parameters (`parse_ctor_dtor_common`) |
| (not mentioned) | `trait Name;` is always bodiless — trait methods are separate `fun Trait.m(): T;` decls |
| (not mentioned) | `borrowed`, `move`, `copy` and `as` are contextual keywords matched by text, not reserved words |
| (not mentioned) | `->` and `=>` are not tokens; the parser reads `Minus`+`Greater` and `Equal`+`Greater` |

Two ambiguities the grammar has to reproduce deliberately:

- **`ident <`** — a comparison, or the head of a generic call / struct literal /
  named-ctor call / value-position generic ref. `try_parse_generic_args()` trial
  parses and commits only when a `>` is found *and* the follow token is one of
  `( { . ; , ) ] } :`. Here that's a declared conflict plus `prec.dynamic`, so
  GLR explores both and the generic reading wins only where it actually parses.
  `prec()` cannot be used — static precedence resolves at generation time and
  would wrongly commit `j < high` to a generic parse.
- **`when x {`** — `m_suppress_struct_literal` bans struct literals across the
  whole discriminant, so the discriminant is modelled as a self-contained
  postfix chain that can't reach `struct_literal`.

Inline `fun` members in a struct body are accepted by the grammar because
`parser.cpp` accepts them, but they are **not** a working language feature: sema
never registers them, so calling one is a "no member" error and merely declaring
one crashes IR generation. Keeping them parseable means such code still
highlights and the LSP reports the real diagnostic.

## Validation

The grammar parses the full corpus — all of `examples/` plus every Roxy snippet
embedded in `tests/e2e` and `tests/unit` (1319 files) — with zero ERROR or
MISSING nodes.

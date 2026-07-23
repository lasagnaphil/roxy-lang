# Function Overloading

User-facing function overloading: a free-function name may have multiple
definitions differing in parameter types or arity, in Roxy source and in the
native registry. `print` is the flagship consumer — it is an overload set with
one member per Printable primitive kind plus a Printable fallback for
everything else.

```roxy
fun describe(x: i32): string  { return f"int {x}"; }
fun describe(s: string): string { return f"str {s}"; }
fun describe(x: i32, y: i32): string { return f"pair {x} {y}"; }

print(42);          // $ol$print$i32
print(my_struct);   // fallback: print(my_struct.to_string())
```

## Scope

- **Free functions and natives only.** Struct methods, constructors,
  destructors, and trait methods stay one-per-name (the existing duplicate
  diagnostics are unchanged).
- **Either generic or overloaded, never both**: a name with a generic template
  cannot also have concrete definitions (checked in both directions —
  `register_fun_signature` and `collect_type_declarations`).
- **`main` cannot be overloaded** (the host invokes it by bare name).
- **One defining module per set**: `from M import f` imports M's whole set
  under one local name; a local definition colliding with an import is a
  redefinition error, so a set never accretes members across modules.
- Identical parameter lists (even with different return types) are
  **redefinition errors**. This turned "user redefines a builtin native"
  (e.g. `fun print(s: string)`) from a silent shadow into an error;
  *extending* the set with a new signature (`fun print(m: Matrix)`) is allowed.

## Symbol representation (symbol_table.{hpp,cpp})

`Symbol::next_overload` chains members off a **head** symbol. Only the head
enters the lookup cache and the scope's symbol vector — chain members are
reachable exclusively through the head, so `define`/`pop_scope`/shadow-restore
and every existing first-match lookup are untouched. `append_overload` adds a
member (definition order preserved). Overload sets exist only at global scope.

## Mangling: the `$ol$` namespace (mangling.{hpp,cpp})

Members of a set (2+ definitions) get a signature-suffixed flat name:

```
$ol$<name>$<param1>$<param2>...     e.g.  $ol$print$i32, $ol$f$List$i32$string
```

- The reserved **leading `$`** is what makes it collision-proof: user
  identifiers can't start with `$`, and every other mangled namespace
  (methods `S$$m`, generic instances `name$i32`, module-locals `mod::name`)
  starts with a user identifier — so `$ol$print$i32` can never alias the
  generic instantiation `print$i32`.
- Contains no `$$`, so the C emitter's structural parsers
  (`suffix_after_last_dollar_dollar`, `$$get`, `$$resume`) ignore it; C output
  folds it to `_ol_print_i32`.
- Param spellings come from `mangle_type_name` (the same canonical
  type-component speller generics use). Return types are excluded.
- **Single definitions are never mangled** — their behavior (symbol name, IR
  name, `vm_call(&vm, "main")`, embedder lookups) is byte-identical to before
  overloading existed. Module-local mangling composes outside:
  `mod::$ol$f$i32`.

Script members record their mangle in `FunDecl::overload_mangled_name`
(back-filled on every member when a set first reaches size 2); native members
are keyed by it in the registry (`NativeFunctionEntry::name`) with the
source-visible name in `NativeFunctionEntry::source_name`.

## Resolution (semantic.cpp: analyze_overloaded_call)

Intercepted at the `analyze_call_expr` fallback when the callee identifier
resolves to a chained function symbol (all special cases — casts, generics,
constructors, methods — keep precedence; a local variable shadowing the name
still routes to the indirect-call path). Non-overloaded names (chain length 1)
never enter this path.

1. **Phase A** — analyze every argument exactly once (single-shot rule),
   including the out/inout lvalue check and container-subscript element
   re-typing.
2. **Phase B** — filter the chain in definition order:
   - *shape*: arity + per-position out/inout modifier equality;
   - *exact pass*: unsuffixed literals settle on their defaults (int→i32,
     float→f64), then exact type equality per position — `print(42)` picks
     `print(i32)`. Duplicate signatures are definition errors, so at most one
     member can match.
   - *assignable pass* (only if no exact match): with the original
     (unsettled) types, `is_assignable` per position; exactly one candidate
     wins, two+ is an **ambiguity error** (candidates listed), zero is a
     **no-match error** (candidates listed) — after the print fallback below
     gets a chance.
   - A deferred function-ref argument (generic-template or overloaded ref)
     matches any function-typed position and coerces against the winner.
3. **Phase C** — commit: assignability diagnostics + literal coercion against
   the winner's params, noncopyable-argument consumption, and the sema→IR
   annotations: `CallExpr.mangled_name` = the member's flat name (registry
   key for natives), callee `resolved_sym` = the winning member, callee
   `resolved_type` = its function type.

Known determinism edge (documented, intended): `f(i64)` + `f(f32)` called
with literal `42` → settles i32 (no exact match), both assignable →
ambiguity error.

### The print Printable fallback

When no `print` overload matches, the single argument is plain, and
`type_implements_printable(arg)` holds, sema rewrites the call to
`print(arg.to_string())`: a hand-annotated GetExpr+CallExpr wrapper around the
already-analyzed argument (never re-analyzed — single-shot rule; precedent:
`inject_default_method`), resolved to the `$ol$print$string` member. The tree
is indistinguishable from user-written `print(x.to_string())`, so string-temp
lifetimes come free. This is what makes `print(vec)` / `print(color)` /
`print(items)` work for every Printable struct, enum, and container.

## Overloaded references in value position

A bare reference to an overloaded name (`var g = f`) can't pick a member —
`analyze_identifier_expr` sets `IdentifierExpr.is_overloaded_ref` and defers
(mirroring the generic-template-ref deferral). `coerce_overloaded_fun_ref`
fires at the same four sites (var init, return, call arg, struct field) and
picks the member whose interned function type equals the expected type
(pointer equality), stashing the flat name in `IdentifierExpr.mangled_name`
and the member in `resolved_sym`. No target type → "reference to overloaded
function is ambiguous" (the var-init site checks even without an annotation).
This is what keeps `greet_via(print, "hello")` and `var p: fun(string) =
print` working.

## IR / backends

- `build_function` emits members under `overload_mangled_name`; the member's
  return type is read from its own chain symbol (the head's would be the
  wrong member's).
- `gen_call_direct` uses `CallExpr.mangled_name` as the callable name — a
  registry key probes as CallNative, otherwise a plain Call (module-local
  wrapped for non-pub members via the winner's `resolved_sym`). The
  imported-alias rewrite (`original_name`) only applies when sema recorded no
  mangled name.
- `gen_identifier_expr` builds Native/ImportedNative/Script/ImportedScript
  `FunctionRefTarget`s for coerced refs from `resolved_sym` + `mangled_name`.
- The C emitter's static native map carries a row per `$ol$print$*` key →
  `roxy_print_*` runtime functions. There is **no registry entry literally
  named "print"** anymore.

## Modules

- `ModuleExport::symbol_name` carries a member's flat name (empty = same as
  `name`); an overloaded name has one export per member, and
  `find_export`-style first-match consumers must not be used for them.
- `from M import f` imports the whole set (head + chained
  ImportedFunction members carrying `original_name` = flat name).
- Module-qualified calls (`m.f(...)`) to an overloaded export are rejected
  with guidance to use `from ... import` (silently binding the first export
  would miscompile).
- The builtin prelude imports the native sets the same way (print's 8
  members chain under one "print" head).

## Tests

`tests/e2e/test_overloads.cpp` (backend-parametric VM/C + a cross-module
Compiler-API case), `tests/unit/test_semantic.cpp` ("Overloads:" cases:
chain mechanics, mangle spellings, accept/reject diagnostics).

# Generics

Roxy has parametric polymorphism via **monomorphization** — each unique type instantiation generates specialized code, with no runtime type dispatch. Generic functions, generic structs, methods/constructors/destructors on generic structs, local type inference, and trait bounds (instantiation- and definition-site) are all implemented. Not implemented: higher-kinded types, associated types, specialization, const generics.

**Native generic types** (e.g., `List<T>`) are a separate mechanism — `NativeRegistry::register_generic_type`. See [interop.md](interop.md#generic-native-types).

## Syntax

### Generic functions

`<T, ...>` type parameters after the function name; type arguments at the call site are explicit or inferred:

```roxy
fun identity<T>(value: T): T { return value; }
fun first<T, U>(a: T, b: U): T { return a; }

var x: i32 = identity<i32>(42);   // explicit
var y: f64 = identity(3.14);      // inferred: T = f64
```

### Generic structs

```roxy
struct Box<T> { value: T; }
struct Pair<T, U> { first: T; second: U; }

var b: Box<i32> = Box<i32> { value = 42 };  // explicit
var b2: Box<i32> = Box { value = 42 };      // inferred: T = i32
```

Generic struct instances are ordinary types: they can appear in function signatures, including a generic function returning a generic struct:

```roxy
fun unbox(b: Box<i32>): i32 { return b.value; }
fun wrap<T>(v: T): Box<T> { return Box<T> { value = v }; }

var b: Box<i32> = wrap(42);   // inferred: T = i32
```

### Methods, constructors, destructors on generic structs

External methods use `fun StructName<T>.method()`. They are monomorphized with the struct — each concrete instantiation gets its own copy (`Box$i32$$get` vs `Box$f64$$get`), and may call other methods on the same struct:

```roxy
struct Box<T> { value: T; }

fun Box<T>.get(): T { return self.value; }
fun Box<T>.set(v: T) { self.value = v; }

var b: Box<i32> = Box<i32> { value = 0 };
b.set(42);
var v: i32 = b.get();  // 42
```

User-defined constructors and destructors use the same `<T>` syntax:

```roxy
fun new Box<T>(value: T) { self.value = value; }      // default constructor
fun new Box<T>.make(value: T) { self.value = value; } // named constructor
fun delete Box<T>() { /* cleanup */ }

var b: Box<i32> = Box<i32>(42);        // user-defined default constructor
var b2: Box<i32> = Box<i32>.make(42);  // named constructor
```

Method/ctor/dtor templates are registered during `resolve_type_members` and cloned with type substitution during `instantiate_struct()`. A user-defined default constructor suppresses the synthesized default constructor for that instantiation.

## Type inference

When type arguments are omitted, the compiler unifies the template's parameter `TypeExpr` tree against the concrete argument types and binds each type parameter.

```roxy
identity(42);                      // T = i32 (function arg)
first(42, 3.14);                   // T = i32, U = f64
Box { value = 42 };                // T = i32 (struct field)
Pair { first = 10, second = 2.5 }; // T = i32, U = f64
```

Generic inference composes with `var` type inference — the variable's type is deduced from the generic RHS:

```roxy
var b = Box { value = 42 };  // b : Box<i32>
var r = identity(42);        // r : i32
```

**Gotchas:**
- All type parameters must be inferable from arguments or fields. A parameter appearing only in the return type cannot be inferred — `make_default<T>(): T` requires an explicit `make_default<i32>()`.
- Generic struct *constructor calls* (`Box(42)`) do not infer; explicit type args are required (`Box<i32>(42)`).

## Disambiguation: `<>` in expression position

`<` and `>` are also comparison operators, so the parser distinguishes generic syntax with a **trial parse + 1-token lookahead after `>`**:

- **In declarations** (`fun name<`, `struct Name<`): always generic params — unambiguous after the keyword.
- **In type annotations** (after `:`): always generic args — `type_expression()` runs only in type position, where `<` is never comparison.
- **In expression position** (`identifier<types>(` vs `a < b`):
  1. Save parser state (tokens + lexer position).
  2. Try parsing comma-separated type expressions, then `>`.
  3. If `>` is followed by `(`, `{`, or `.` → commit as generic call/literal/named constructor.
  4. Otherwise → restore state, parse `<` as comparison.

## Monomorphization

Each unique instantiation generates specialized code, named by mangling:

```
function<T>       -> function$T
function<T, U>    -> function$T$U
Struct<T>         -> Struct$T
Struct<T>.method  -> Struct$T$$method
```

So `identity<i32>` → `identity$i32`, `Pair<i32, f64>` → `Pair$i32$f64`, `Box<i32>.get` → `Box$i32$$get`.

Slot layout is computed per instantiation: `Pair<i32, i32>` is 2 slots, `Pair<i64, i32>` is 3, `Pair<Point, f64>` is 4 (if `Point` is 2 slots).

### How it works

Generic functions and structs are registered as **templates** in `GenericInstantiator` during Pass 1 (`collect_type_declarations`) — they are not added as regular symbols, and later passes skip them. When an instantiation site is reached (`identity<i32>(42)`, `Box<i32> { ... }`), the entire AST is deep-cloned and every `TypeExpr` naming a type parameter is replaced with the concrete type; compound types like `Box<T>` substitute recursively. After cloning, `resolve_type_expr` rewrites TypeExpr names to mangled names (`Box$i32`) so the IR builder finds the right type.

A post-Pass-3 **worklist loop** processes pending instances (structs first, then functions) until none remain, since generic function bodies can trigger further struct instantiations. A generic struct's fields and method signatures are resolved inline on first instantiation (not deferred) so same-pass users can access them; method/ctor/dtor *bodies* are analyzed later in the worklist.

The core records — `TypeSubstitution` (param names → concrete types), `GenericFunInstance`, and `GenericStructInstance` (mangled name, original/instantiated decls, concrete `Type*`, cloned methods/ctors/dtors) — live in `compiler/generics.hpp`.

## Trait bounds

Type parameters are constrained with `<T: Trait>`; multiple bounds combine with `+`, and bounds may be generic. The compiler checks bounds both where a generic is *used* (Phase A) and where a bounded generic is *defined* (Phase B). See [traits.md](traits.md) for trait declarations.

```roxy
fun identity_printable<T: Printable>(value: T): T { return value; }
fun identity_both<T: Printable + Hash>(value: T): T { return value; }
fun apply_scale<T: Scalable<Vec2>>(v: T): i32 { return 1; }
struct HashBox<T: Hash> { value: T; }
```

### Instantiation-site checking (Phase A)

At every instantiation site, the compiler verifies the concrete type satisfies each bound. The five sites are: explicit/inferred generic function calls, explicit generic struct constructor calls, and explicit/inferred generic struct literals.

```roxy
identity_printable<i32>(42);      // OK: i32 implements Printable
identity_printable<MyStruct>(s);  // ERROR if MyStruct doesn't implement Printable
HashBox<i32> { value = 42 };      // OK: i32 implements Hash
```

Self-referential bounds like `<T: Add<T>>` work — `T` inside `Add<T>` is substituted with the concrete type during checking.

### Definition-site checking (Phase B)

When a generic function has at least one bounded type parameter, its body is analyzed once at the definition site (before any instantiation), with type parameters as abstract `TypeParam` types. Method calls and operators on bounded parameters are validated against the declared bounds:

```roxy
trait Greetable;
fun Greetable.greet(): i32;

fun call_greet<T: Greetable>(v: T): i32 { return v.greet(); }  // OK
fun bad<T: Greetable>(v: T): i32 { return v.unknown(); }       // ERROR: no 'unknown' in bounds
```

`analyze_generic_template_body()` sets up the bounds context; method calls on `TypeParam` dispatch to `lookup_type_param_method()` (searches the bounds), and operator resolution consults the bounds similarly. Trait method signatures substitute `Self` → the type parameter and the trait's type params → the bound's type args. **Unbounded** templates are still only checked at instantiation time. When both phases flag the same issue, the definition-site error appears first and is more useful.

### Pipeline

- **Parsing:** `parse_type_params()` parses `: Trait1 + Trait2<Args>` after each parameter name.
- **Resolution:** `resolve_generic_bounds()` (Pass 1.9) resolves bound expressions to `TraitBound` records in `GenericInstantiator`.
- **Phase A:** `check_type_arg_bounds()` at the five instantiation sites.
- **Phase B:** `analyze_generic_template_body()` during `analyze_function_bodies()` for bounded generic functions.

## Grammar

```
generic_params  -> "<" type_param_list ">" ;
type_param_list -> type_param ( "," type_param )* ;
type_param      -> Identifier ( ":" trait_bounds )? ;
trait_bounds    -> trait_bound ( "+" trait_bound )* ;
trait_bound     -> Identifier generic_args? ;

generic_args    -> "<" type_list ">" ;
type_list       -> type_expr ( "," type_expr )* ;

fun_decl        -> ( "pub" )? "fun" Identifier generic_params?
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

struct_decl     -> "struct" Identifier generic_params?
                   ( ":" Identifier generic_args? )?
                   "{" field_decl* "}" ;

type_expr       -> ( "uniq" | "ref" | "weak" )? Identifier generic_args? ;
```

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/generics.hpp` | `GenericInstantiator`, `TypeSubstitution`, instance records |
| `src/roxy/compiler/generics.cpp` | Instantiation, AST cloning with substitution, name mangling |
| `include/roxy/compiler/ast.hpp` | `TypeParam`, `type_params`/`type_args` on decls and exprs |
| `include/roxy/compiler/types.hpp` | `TypeKind::TypeParam` for unresolved type parameters |
| `include/roxy/shared/lexer.hpp` | `save_position()` / `restore_position()` for trial-parse backtracking |
| `src/roxy/compiler/semantic.cpp` | Template registration, instantiation, type resolution, bounds checking |
| `src/roxy/compiler/ir_builder.cpp` | IR generation for generic instances |
| `tests/e2e/test_generics.cpp` | E2E tests (incl. Phase A/B trait bounds and generic struct methods) |

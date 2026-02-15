# Generics

Generics enable writing code that works with multiple types while maintaining static type safety. Roxy uses **monomorphization** — each unique type instantiation generates specialized code.

## Implementation Status

**Implemented (Phase 1 + 2):**
- Generic functions with explicit type arguments
- Generic structs with explicit type arguments
- Monomorphization with AST cloning and type substitution
- Angle bracket syntax with trial-parse disambiguation
- Name mangling (`identity<i32>` → `identity$i32`, `Box<i32>` → `Box$i32`)
- Generic structs as function parameters and return types
- Generic functions returning generic structs
- Synthesized default constructors for generic struct instances

**Not yet implemented:**
- Type inference (type arguments must be explicit)
- Trait bounds on type parameters
- Generic methods on generic structs
- Generic constructors/destructors
- Higher-kinded types, associated types, specialization, const generics

## Syntax

### Generic Functions

```roxy
fun identity<T>(value: T): T {
    return value;
}

fun first<T, U>(a: T, b: U): T {
    return a;
}

// Usage — explicit type arguments required
var x: i32 = identity<i32>(42);
var y: f64 = identity<f64>(3.14);
```

### Generic Structs

```roxy
struct Box<T> {
    value: T;
}

struct Pair<T, U> {
    first: T;
    second: U;
}

// Usage
var b: Box<i32> = Box<i32> { value = 42 };
var p: Pair<i32, f64> = Pair<i32, f64> { first = 10, second = 2.5 };
```

### Generic Structs in Function Signatures

```roxy
struct Box<T> { value: T; }

// Non-generic function taking/returning a generic struct instance
fun unbox(b: Box<i32>): i32 { return b.value; }
fun make_box(v: i32): Box<i32> { return Box<i32> { value = v }; }

// Generic function returning a generic struct
fun wrap<T>(v: T): Box<T> { return Box<T> { value = v }; }

var b: Box<i32> = wrap<i32>(42);
```

## Disambiguation: `<>` in Expression Position

`<` and `>` are also comparison operators. The parser uses **trial parse with 1-token lookahead after `>`**:

**In declarations** (`fun name<`, `struct Name<`): Always generic params — unambiguous after keywords.

**In type annotations** (after `:`): Always generic args — `type_expression()` is only called in type position where `<` is never a comparison.

**In expression position** (`identifier<types>(` vs `a < b`):
1. Save parser state (tokens + lexer position)
2. Try parsing as comma-separated type expressions, then `>`
3. If `>` is followed by `(` or `{` → commit as generic call/literal
4. Otherwise → restore state, parse `<` as comparison operator

## Monomorphization

Each unique type instantiation generates specialized code:

```roxy
identity<i32>(42);     // Generates identity$i32
identity<f64>(3.14);   // Generates identity$f64
Box<i32> { value = 1 } // Generates struct type Box$i32
```

### Name Mangling

```
function<T>       -> function$T
function<T, U>    -> function$T$U
Struct<T>         -> Struct$T
Struct<T>.method  -> Struct$T$$method
```

Examples:
- `identity<i32>` → `identity$i32`
- `Box<string>` → `Box$string`
- `Pair<i32, f64>` → `Pair$i32$f64`

### Struct Layout

Generic structs compute slot layout per instantiation:

```roxy
struct Pair<T, U> { first: T; second: U; }

// Pair<i32, i32>: 2 slots
// Pair<i64, i32>: 3 slots
// Pair<Point, f64>: 4 slots (if Point is 2 slots)
```

## Implementation Details

### Compiler Pipeline

1. **Parser**: Parses `<T>` type params in declarations, `<i32>` type args in types and expressions. Uses trial parse for expression-position disambiguation.
2. **Semantic Pass 1** (`collect_type_declarations`): Registers generic functions/structs as templates in `GenericInstantiator`. They are NOT added as regular symbols.
3. **Semantic Pass 2** (`resolve_type_members`): Skips generic templates. Non-generic functions with generic struct params/returns trigger struct instantiation here.
4. **Semantic Pass 3** (`analyze_declarations`): Skips generic templates. When a call like `identity<i32>(42)` is encountered, instantiates via AST cloning with type substitution.
5. **Worklist loop**: After pass 3, processes pending generic instances (structs first, then functions) in a loop until no more are pending. Generic function bodies may trigger further struct instantiations.
6. **IR Builder**: Skips generic templates. Builds IR for all analyzed generic instances.

### Key Types

```cpp
// Maps type parameter names to concrete types
struct TypeSubstitution {
    Span<StringView> param_names;    // ["T", "U"]
    Span<Type*> concrete_types;      // [i32_type, f64_type]
};

// A concrete instantiation of a generic function
struct GenericFunInstance {
    StringView mangled_name;         // "identity$i32"
    Decl* original_decl;
    TypeSubstitution substitution;
    Decl* instantiated_decl;         // Cloned + substituted AST
    bool is_analyzed;
};

// A concrete instantiation of a generic struct
struct GenericStructInstance {
    StringView mangled_name;         // "Box$i32"
    Decl* original_decl;
    TypeSubstitution substitution;
    Decl* instantiated_decl;
    Type* concrete_type;             // The concrete struct Type*
    bool is_analyzed;
};
```

### AST Cloning with Substitution

When instantiating `identity<i32>`, the entire function AST is deep-cloned. During cloning, all `TypeExpr` nodes with names matching type parameters (e.g., `T`) are replaced with the concrete type name (e.g., `i32`). For compound types like `Box<T>`, the type args are recursively substituted.

After cloning, `resolve_type_expr` updates TypeExpr names to mangled names (e.g., `Box<i32>` TypeExpr gets `name = "Box$i32"`) so the IR builder can look up the correct type.

### Generic Struct Field Resolution

When a generic struct is first instantiated via `resolve_type_expr`, its fields are resolved **immediately inline** rather than deferred to the worklist. This ensures that functions using the struct (in the same or later analysis passes) can access field information.

## Grammar

```
generic_params  -> "<" type_param_list ">" ;
type_param_list -> type_param ( "," type_param )* ;
type_param      -> Identifier ;

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
| `include/roxy/compiler/generics.hpp` | GenericInstantiator, TypeSubstitution, instance types |
| `src/roxy/compiler/generics.cpp` | Instantiation, AST cloning with substitution, name mangling |
| `include/roxy/compiler/ast.hpp` | TypeParam, type_params on FunDecl/StructDecl, type_args on TypeExpr/CallExpr/StructLiteralExpr |
| `include/roxy/compiler/types.hpp` | TypeKind::TypeParam for unresolved type parameters |
| `include/roxy/shared/lexer.hpp` | save_position()/restore_position() for parser backtracking |
| `src/roxy/compiler/semantic.cpp` | Generic template registration, instantiation, type resolution |
| `src/roxy/compiler/ir_builder.cpp` | IR generation for generic instances |
| `tests/e2e/generics_test.cpp` | E2E tests (15 test cases) |

## Limitations

1. **No type inference**: Type arguments must always be explicit (`identity<i32>(42)`, not `identity(42)`)
2. **No trait bounds**: `<T: Printable>` syntax is not yet supported
3. **No generic methods**: Methods on generic structs are not yet supported
4. **No higher-kinded types**: Can't abstract over type constructors
5. **No associated types**: Traits can't define type members
6. **No specialization**: Can't provide different implementations for specific types
7. **No const generics**: Can't parameterize by values

## Future Work

1. **Type inference** — Infer type arguments from usage context
2. **Trait bounds** — `<T: Printable>` for constrained polymorphism
3. **Generic methods** — Methods on generic structs (`fun Box.get<T>(): T`)
4. **Generic constructors** — `fun new Box<T>(value: T)`

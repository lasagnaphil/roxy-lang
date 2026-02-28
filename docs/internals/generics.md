# Generics

Generics enable writing code that works with multiple types while maintaining static type safety. Roxy uses **monomorphization** — each unique type instantiation generates specialized code.

## Implementation Status

**Implemented (Phase 1 + 2 + 3 + 4 + 5 + 6 + 7):**
- Generic functions with explicit type arguments
- Generic structs with explicit type arguments
- Monomorphization with AST cloning and type substitution
- Angle bracket syntax with trial-parse disambiguation
- Name mangling (`identity<i32>` → `identity$i32`, `Box<i32>` → `Box$i32`)
- Generic structs as function parameters and return types
- Generic functions returning generic structs
- Synthesized default constructors for generic struct instances
- Generic traits with type parameters (`trait Add<Rhs>`, see `traits.md`)
- Local type inference for generic type arguments (function calls and struct literals)
- Trait bounds on type parameters (Phase A: instantiation-site checking)
- Trait bounds body checking (Phase B: definition-site checking of generic template bodies)
- User-defined external methods on generic structs (`fun Box<T>.get(): T`)

**Not yet implemented:**
- User-defined generic constructors/destructors
- Higher-kinded types, associated types, specialization, const generics

**Native generic types** (e.g., `List<T>`) are supported via `NativeRegistry::register_generic_type`. See [interop.md](interop.md#generic-native-types) for details.

## Syntax

### Generic Functions

```roxy
fun identity<T>(value: T): T {
    return value;
}

fun first<T, U>(a: T, b: U): T {
    return a;
}

// Usage — explicit or inferred type arguments
var x: i32 = identity<i32>(42);   // explicit
var y: f64 = identity(3.14);      // inferred: T = f64
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

// Usage — explicit or inferred type arguments
var b: Box<i32> = Box<i32> { value = 42 };               // explicit
var b2: Box<i32> = Box { value = 42 };                    // inferred: T = i32
var p: Pair<i32, f64> = Pair { first = 10, second = 2.5 }; // inferred: T = i32, U = f64
```

### Generic Structs in Function Signatures

```roxy
struct Box<T> { value: T; }

// Non-generic function taking/returning a generic struct instance
fun unbox(b: Box<i32>): i32 { return b.value; }
fun make_box(v: i32): Box<i32> { return Box<i32> { value = v }; }

// Generic function returning a generic struct
fun wrap<T>(v: T): Box<T> { return Box<T> { value = v }; }

var b: Box<i32> = wrap(42);   // inferred: T = i32
```

### Methods on Generic Structs

External methods can be defined on generic structs using the `fun StructName<T>.method()` syntax. Methods are monomorphized along with the struct — each concrete instantiation gets its own copy:

```roxy
struct Box<T> { value: T; }

fun Box<T>.get(): T { return self.value; }
fun Box<T>.set(v: T) { self.value = v; }

// Usage
var b: Box<i32> = Box<i32> { value = 0 };
b.set(42);
var v: i32 = b.get();  // v = 42

// Multiple instantiations generate separate methods
var b2: Box<f64> = Box<f64> { value = 3.14 };
var v2: f64 = b2.get();  // calls Box$f64$$get, not Box$i32$$get
```

Methods can call other methods on the same generic struct:

```roxy
struct Counter<T> { value: T; count: i32; }

fun Counter<T>.get_count(): i32 { return self.count; }
fun Counter<T>.is_empty(): bool { return self.get_count() == 0; }
```

## Type Inference

When type arguments are omitted, the compiler attempts **local type inference** by unifying the template parameter types against the concrete argument types.

### Function Calls

For a generic function call without explicit type args, the compiler analyzes each argument expression to determine its concrete type, then walks the template's parameter `TypeExpr` tree in parallel with the concrete `Type*` to bind type parameters:

```roxy
fun identity<T>(value: T): T { return value; }
fun first<T, U>(a: T, b: U): T { return a; }

identity(42);         // T = i32 (from argument type)
first(42, 3.14);      // T = i32, U = f64
```

### Struct Literals

For generic struct literals without explicit type args, the compiler matches each field initializer against the template's field declarations:

```roxy
struct Box<T> { value: T; }
struct Pair<T, U> { first: T; second: U; }

Box { value = 42 }                    // T = i32
Pair { first = 10, second = 2.5 }     // T = i32, U = f64
```

### Composing with Variable Type Inference

Generic inference composes with `var` type inference — the variable's type is deduced from the generic RHS:

```roxy
var b = Box { value = 42 };           // var type inferred as Box<i32>
var result = identity(42);            // var type inferred as i32
var b2 = wrap(42);                    // var type inferred as Box<i32>
var p = Pair { first = 10, second = 2.5 };  // var type inferred as Pair<i32, f64>
```

### Limitations

- All type parameters must be inferable from function arguments or struct field values. If a type parameter only appears in the return type, inference fails:
  ```roxy
  fun make_default<T>(): T { ... }
  make_default();         // ERROR: cannot infer T
  make_default<i32>();    // OK: explicit type arg
  ```
- Generic struct *constructor calls* (e.g., `Box(42)`) are not supported for inference since user-defined generic constructors are not yet implemented.

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
    Vector<Decl*> instantiated_methods;  // Cloned external method DeclMethod nodes
};
```

### AST Cloning with Substitution

When instantiating `identity<i32>`, the entire function AST is deep-cloned. During cloning, all `TypeExpr` nodes with names matching type parameters (e.g., `T`) are replaced with the concrete type name (e.g., `i32`). For compound types like `Box<T>`, the type args are recursively substituted.

After cloning, `resolve_type_expr` updates TypeExpr names to mangled names (e.g., `Box<i32>` TypeExpr gets `name = "Box$i32"`) so the IR builder can look up the correct type.

### Generic Struct Field and Method Resolution

When a generic struct is first instantiated via `resolve_type_expr`, its fields and method signatures are resolved **immediately inline** via `resolve_generic_struct_fields()` rather than deferred to the worklist. This ensures that functions using the struct (in the same or later analysis passes) can access field information and call methods.

External method templates (`fun Box<T>.get(): T`) are registered during `resolve_type_members` and cloned with type substitution during `instantiate_struct()`. The cloned method declarations are stored in `GenericStructInstance::instantiated_methods`. Method **bodies** are analyzed later in the worklist loop.

## Trait Bounds

Type parameters can be constrained with trait bounds using the `<T: Trait>` syntax. The compiler checks that concrete type arguments satisfy all bounds at every instantiation site (function calls, struct literals, constructor calls).

### Syntax

```roxy
// Single bound
fun identity_printable<T: Printable>(value: T): T { return value; }

// Multiple bounds with +
fun identity_both<T: Printable + Hash>(value: T): T { return value; }

// Generic trait bound
fun apply_scale<T: Scalable<Vec2>>(v: T): i32 { return 1; }

// Bounds on generic structs
struct HashBox<T: Hash> { value: T; }

// Bounds with multiple type params
fun process<T: Printable, U: Hash>(a: T, b: U): i32 { return 0; }
```

### Instantiation-Site Checking (Phase A)

Bounds are resolved after all types and traits are registered. At each instantiation site, the compiler verifies that the concrete type satisfies every bound:

```roxy
identity_printable<i32>(42);     // OK: i32 implements Printable
identity_printable<MyStruct>(s); // ERROR if MyStruct doesn't implement Printable

HashBox<i32> { value = 42 };    // OK: i32 implements Hash
HashBox<MyStruct> { value = s }; // ERROR if MyStruct doesn't implement Hash
```

Self-referential bounds like `<T: Add<T>>` are supported — `T` in `Add<T>` is substituted with the concrete type during checking.

### Definition-Site Checking (Phase B)

When a generic function has at least one bounded type parameter, its body is analyzed at the **definition site** — before any instantiation. Method calls and operator usage on bounded type parameters are validated against the declared trait bounds:

```roxy
trait Greetable;
fun Greetable.greet(): i32;

fun call_greet<T: Greetable>(v: T): i32 {
    return v.greet();    // OK: greet() is in Greetable
}

fun bad<T: Greetable>(v: T): i32 {
    return v.unknown();  // ERROR: no method 'unknown' in trait bounds for T
}
```

**How it works:**
- `analyze_generic_template_body()` sets up a bounds context (`m_active_type_param_bounds`, `m_active_type_params`) and analyzes the function body with type parameters as abstract `TypeParam` types
- `resolve_type_expr()` resolves type param names to `TypeParam` types during template body analysis
- `analyze_call_expr()` dispatches method calls on `TypeParam` types to `lookup_type_param_method()`, which searches trait bounds for matching methods
- `try_resolve_binary_op()` / `try_resolve_unary_op()` similarly consult trait bounds for operator dispatch
- Trait method signatures use `Self` and trait type params which are substituted: `Self` → the type parameter, trait type params → the bound's type args
- `is_assignable()` / `check_assignable()` treat `TypeParam` types as assignable to themselves (same name/index)

**Unbounded templates** (no type param has any bound) are still only checked at instantiation time.

**Duplicate checking:** Both definition-site and instantiation-site checking may report errors for the same issue. The definition-site error appears first and is more useful.

### Implementation

- **Parsing:** `parse_type_params()` parses `: Trait1 + Trait2<Args>` after each type param name
- **Resolution:** `resolve_generic_bounds()` (Pass 1.9) resolves bound expressions to `TraitBound` records stored in `GenericInstantiator`
- **Phase A checking:** `check_type_arg_bounds()` called at all 5 instantiation sites: explicit/inferred generic function calls, explicit generic struct constructor calls, explicit/inferred generic struct literals
- **Phase B checking:** `analyze_generic_template_body()` called during `analyze_function_bodies()` for bounded generic functions, validates method calls, operators, and type compatibility against bounds

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
| `include/roxy/compiler/generics.hpp` | GenericInstantiator, TypeSubstitution, instance types |
| `src/roxy/compiler/generics.cpp` | Instantiation, AST cloning with substitution, name mangling |
| `include/roxy/compiler/ast.hpp` | TypeParam, type_params on FunDecl/StructDecl, type_args on TypeExpr/CallExpr/StructLiteralExpr |
| `include/roxy/compiler/types.hpp` | TypeKind::TypeParam for unresolved type parameters |
| `include/roxy/shared/lexer.hpp` | save_position()/restore_position() for parser backtracking |
| `src/roxy/compiler/semantic.cpp` | Generic template registration, instantiation, type resolution |
| `src/roxy/compiler/ir_builder.cpp` | IR generation for generic instances |
| `tests/e2e/test_generics.cpp` | E2E tests (66 test cases including Phase A + Phase B trait bounds and generic struct methods) |

## Limitations

1. **Limited type inference**: Type arguments can be inferred from function args and struct fields, but not from return type context alone
2. **No user-defined generic constructors/destructors**: Native types support these via `bind_generic_constructor`/`bind_generic_destructor`, but user-defined ones are not yet supported
3. **No higher-kinded types**: Can't abstract over type constructors
4. **No associated types**: Traits can't define type members
5. **No specialization**: Can't provide different implementations for specific types
6. **No const generics**: Can't parameterize by values

## Future Work

1. **User-defined generic constructors** — `fun new Box<T>(value: T)`
2. **Trait implementations on generic structs** — `fun Box<T>.print() for Printable`

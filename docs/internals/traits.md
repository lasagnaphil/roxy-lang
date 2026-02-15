# Traits

Traits provide a way to define shared behavior across types. They enable polymorphism without function overloading.

**Implemented features:** Trait declarations, required/default methods, trait implementations (`for Trait`), trait inheritance, `Self` type, and operator dispatch for comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`).

**Not yet implemented:** Generic functions with trait bounds (`<T: Trait>`), arithmetic/bitwise operator traits (require generics), traits on primitive types, standard library traits.

## Syntax Overview

Roxy uses a free-floating syntax for traits, consistent with struct methods:

```roxy
// Declare trait
trait Printable;

// Required method (no body)
fun Printable.print();

// Default method (has body)
fun Printable.println() {
    self.print();
    print_str("\n");
}
```

**Rule:** No body = required, has body = default implementation.

## Trait Declaration

```roxy
trait TraitName;
```

Declares a new trait. Trait methods are defined separately using free-floating syntax.

## Trait Methods

### Required Methods

Methods without a body must be implemented by any type that implements the trait:

```roxy
trait Comparable;
fun Comparable.compare(other: Self): i32;  // Self = implementing type
```

### Default Methods

Methods with a body provide default implementations that can be optionally overridden:

```roxy
trait Comparable;
fun Comparable.compare(other: Self): i32;

// Defaults built on required method
fun Comparable.lt(other: Self): bool {
    return self.compare(other) < 0;
}

fun Comparable.gt(other: Self): bool {
    return self.compare(other) > 0;
}

fun Comparable.eq(other: Self): bool {
    return self.compare(other) == 0;
}

fun Comparable.lte(other: Self): bool {
    return self.compare(other) <= 0;
}

fun Comparable.gte(other: Self): bool {
    return self.compare(other) >= 0;
}
```

## Implementing Traits

Use the `for Trait` suffix to implement a trait method for a type:

```roxy
struct Point { x: i32; y: i32; }

// Implement required method
fun Point.compare(other: Point): i32 for Comparable {
    var self_mag: i32 = self.x * self.x + self.y * self.y;
    var other_mag: i32 = other.x * other.x + other.y * other.y;
    return self_mag - other_mag;
}

// Now Point has: compare(), lt(), gt(), eq(), lte(), gte()
```

### Overriding Default Methods

```roxy
// Override a default implementation
fun Point.eq(other: Point): bool for Comparable {
    return self.x == other.x && self.y == other.y;
}
```

### Missing Implementations

If a required method is not implemented, the compiler reports an error:

```
error: incomplete trait implementation
  --> main.roxy:15:1
   |
   | fun Point.lt(other: Point): bool for Comparable {
   |     ^^^^^ trait 'Comparable' requires 'compare' which is not implemented for 'Point'
   |
   = note: required method: fun Comparable.compare(other: Self): i32
```

## Trait Inheritance

Traits can extend other traits:

```roxy
trait Printable;
fun Printable.print();

trait DebugPrintable : Printable;
fun DebugPrintable.debug_print() {
    print_str("[DEBUG] ");
    self.print();
}
```

Implementing `DebugPrintable` requires also implementing `Printable`.

## Generic Functions with Trait Bounds

Functions can require types to implement specific traits:

```roxy
fun debug<T: Printable>(value: T) {
    print_str("[DEBUG] ");
    value.print();
    print_str("\n");
}

// Multiple trait bounds
fun compare_and_print<T: Printable + Comparable>(a: T, b: T) {
    a.print();
    if (a.lt(b)) {
        print_str(" < ");
    } else {
        print_str(" >= ");
    }
    b.print();
}
```

## The Universal `print()` Function

With traits, a universal `print()` function can be defined:

```roxy
// In prelude/builtin
trait Printable;
fun Printable.print();
fun Printable.println() {
    self.print();
    print_str("\n");
}

// Builtin implementations
fun i32.print() for Printable {
    print_i32(self);
}

fun i64.print() for Printable {
    print_i64(self);
}

fun f64.print() for Printable {
    print_f64(self);
}

fun bool.print() for Printable {
    if (self) { print_str("true"); }
    else { print_str("false"); }
}

fun string.print() for Printable {
    print_str(self);
}

// Universal print function
fun print<T: Printable>(value: T) {
    value.print();
}
```

Usage:

```roxy
fun main(): i32 {
    print(42);           // Works!
    print(3.14);         // Works!
    print("hello");      // Works!
    print(true);         // Works!

    var p: Point = Point { x = 10, y = 20 };
    print(p);            // Works if Point implements Printable

    return 0;
}
```

## Traits on Built-in Containers

Built-in parameterized types (see `generics.md`) can also implement traits:

```roxy
// Built-in array implements Printable if element type does
fun T[].print() for Printable where T: Printable {
    print_str("[");
    for (var i: i32 = 0; i < array_len(self); i = i + 1) {
        if (i > 0) { print_str(", "); }
        self[i].print();
    }
    print_str("]");
}

// Option<T> implements Printable if T does
fun Option.print<T>() for Printable where T: Printable {
    if (self.has_value()) {
        print_str("Some(");
        self.unwrap().print();
        print_str(")");
    } else {
        print_str("None");
    }
}
```

These implementations are provided by the compiler/standard library, not monomorphized per element type.

## Complete Example

```roxy
// === Trait declarations ===
trait Printable;
fun Printable.print();
fun Printable.println() {
    self.print();
    print_str("\n");
}

trait Comparable;
fun Comparable.compare(other: Self): i32;
fun Comparable.lt(other: Self): bool { return self.compare(other) < 0; }
fun Comparable.gt(other: Self): bool { return self.compare(other) > 0; }
fun Comparable.eq(other: Self): bool { return self.compare(other) == 0; }

// === Struct definition ===
struct Score {
    value: i32;
    name: string;
}

// === Regular method ===
fun Score.double(): i32 {
    return self.value * 2;
}

// === Trait implementations ===
fun Score.print() for Printable {
    print_str(self.name);
    print_str(": ");
    print_i32(self.value);
}

fun Score.compare(other: Score): i32 for Comparable {
    return self.value - other.value;
}

// === Generic function ===
fun print_winner<T: Printable + Comparable>(a: T, b: T) {
    print_str("Winner: ");
    if (a.gt(b)) {
        a.println();
    } else {
        b.println();
    }
}

// === Usage ===
fun main(): i32 {
    var alice: Score = Score { value = 150, name = "Alice" };
    var bob: Score = Score { value = 120, name = "Bob" };

    alice.println();           // Alice: 150
    bob.println();             // Bob: 120

    print_winner(alice, bob);  // Winner: Alice: 150

    return 0;
}
```

## Grammar

```
trait_decl      -> "trait" Identifier ( ":" Identifier )? ";" ;

trait_method    -> "fun" Identifier "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

impl_method     -> "fun" Identifier "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   "for" Identifier
                   block ;

type_param      -> Identifier ( ":" trait_bounds )? ;
trait_bounds    -> Identifier ( "+" Identifier )* ;
generic_params  -> "<" type_param ( "," type_param )* ">" ;
```

## Standard Traits (Planned)

| Trait | Methods | Purpose |
|-------|---------|---------|
| `Printable` | `print`, `println` | Text output |
| `ToString` | `to_string` | String conversion |
| `Hash` | `hash` | Hash computation |
| `Clone` | `clone` | Deep copying |
| `Default` | `default` | Default values |
| `Iterator` | `next`, `has_next` | Iteration |

For operator traits (`Eq`, `Ord`, `Add`, `Sub`, etc.), see `operator-overloading.md`.

## Implementation Strategy

### Method Resolution

1. Check if type has direct implementation (`fun Type.method() for Trait`)
2. Fall back to trait's default implementation (deep-cloned and injected per struct)
3. Error if required method is missing

### Name Mangling

Trait methods are stored on the struct and use standard method mangling:

```
Type$$method
```

Examples:
- `Point$$eq`
- `Score$$compare`

This works because each struct has at most one implementation of each method name. If two traits define the same method name, the compiler reports an error.

### Default Method Injection

Default methods (trait methods with bodies) are deep-cloned for each implementing struct that doesn't override them. The AST is cloned so that `resolved_type` annotations are independent per concrete `Self` type. These are added as synthetic declarations processed alongside regular methods.

### Operator Dispatch

For comparison operators on struct types, the compiler checks if the struct has the corresponding method (e.g., `eq` for `==`, `lt` for `<`) and rewrites the operator into a method call. This happens in both semantic analysis (type checking) and IR generation (code emission).

## Comparison with Alternatives

| Approach | Pros | Cons |
|----------|------|------|
| Function overloading | Simple concept | Name mangling, ambiguity |
| Traits | Extensible, clean | More complex to implement |
| Duck typing | Flexible | No static checking |

Traits provide a middle ground: static type safety with user-extensible polymorphism.

## Dependencies

Implementing traits requires:

1. **Trait declarations** - New AST node for `trait Name;`
2. **Trait method declarations** - Track required vs default methods
3. **Implementation tracking** - Map of (Type, Trait) → implemented methods
4. **`Self` type** - Referring to the implementing type in signatures
5. **Method resolution** - Check trait implementations when calling methods
6. **Generics** (optional) - For generic functions with trait bounds (`<T: Trait>`)

See `generics.md` for discussion of compile-time trade-offs and the recommendation to implement traits before full generics.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/types.hpp` | `TraitMethodInfo`, `TraitTypeInfo`, trait type kind |
| `include/roxy/compiler/ast.hpp` | `DeclTrait` AST node, `trait_name` on `MethodDecl` |
| `src/roxy/compiler/parser.cpp` | `trait_declaration()`, `for Trait` parsing |
| `src/roxy/compiler/semantic.cpp` | Trait analysis, validation, default method injection |
| `src/roxy/compiler/ir_builder.cpp` | Operator dispatch, synthetic decl processing |
| `tests/e2e/traits_test.cpp` | E2E tests |
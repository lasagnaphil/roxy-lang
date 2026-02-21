# Traits

Traits provide a way to define shared behavior across types. They enable polymorphism without function overloading.

**Implemented features:** Trait declarations, required/default methods, trait implementations (`for Trait`), trait inheritance, `Self` type, operator dispatch for comparison operators (`==`, `!=`, `<`, `<=`, `>`, `>=`), and generic traits with type parameters (`trait Add<Rhs>`).

**Not yet implemented:** Generic functions with trait bounds (`<T: Trait>`), arithmetic/bitwise operator dispatch (generic trait infrastructure exists, dispatch not wired), traits on primitive types, standard library traits.

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
    print("\n");
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
    print("[DEBUG] ");
    self.print();
}
```

Implementing `DebugPrintable` requires also implementing `Printable`.

## Generic Traits

Traits can have type parameters, enabling mixed-type operations:

```roxy
trait Add<Rhs>;
fun Add.add(other: Rhs): Self;

trait Convert<From, To>;
fun Convert.convert(input: From): To;
```

### Implementing Generic Traits

Specify concrete type arguments in the `for` clause:

```roxy
struct Vec2 { x: i32; y: i32; }

// Same-type: Rhs = Vec2
fun Vec2.add(other: Vec2): Vec2 for Add<Vec2> {
    return Vec2 { x = self.x + other.x, y = self.y + other.y };
}

// Mixed-type: Rhs = i32
trait Mul<Rhs>;
fun Mul.mul(other: Rhs): Self;

fun Vec2.mul(scalar: i32): Vec2 for Mul<i32> {
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}
```

### Default Methods with Type Parameters

Default methods can use trait type parameters. When injected into a struct, type parameters are substituted with the concrete type arguments:

```roxy
trait Add<Rhs>;
fun Add.add(other: Rhs): Self;
fun Add.add_twice(other: Rhs): Self {
    return self.add(other).add(other);   // Rhs is substituted when injected
}

struct Num { val: i32; }

fun Num.add(other: Num): Num for Add<Num> {
    return Num { val = self.val + other.val };
}
// Num now has add_twice() with Rhs=Num substituted in the body
```

### Multi-Parameter Generic Traits

```roxy
trait Convert<From, To>;
fun Convert.convert(input: From): To;

struct Converter { factor: i32; }

fun Converter.convert(input: i32): i32 for Convert<i32, i32> {
    return input * self.factor;
}
```

### Error Cases

The compiler validates generic trait usage:
- **Wrong type arg count:** `for Add` when `Add<Rhs>` expects 1 type argument → error
- **Type args on non-generic trait:** `for Eq<i32>` when `Eq` has no type parameters → error

### Constraints

- A struct can implement a given generic trait only once per set of type arguments
- No default type arguments (must always specify `for Add<Vec2>`, not `for Add`)
- Generic trait inheritance is not yet supported (e.g., `trait AddAssign<Rhs> : Add<Rhs>`)
## Trait Bounds on Type Parameters (Bounded Quantification)

Type parameters can be constrained with trait bounds, requiring concrete types to implement specified traits at every instantiation site:

```roxy
// Single bound
fun identity_printable<T: Printable>(value: T): T {
    return value;
}

// Multiple bounds with +
fun identity_both<T: Printable + Hash>(value: T): T {
    return value;
}

// Generic trait bound
fun apply_scale<T: Scalable<Vec2>>(v: T): i32 {
    return 1;
}

// Bounds on generic structs
struct HashBox<T: Hash> {
    value: T;
}
```

**Phase A (current):** Bounds are checked at instantiation sites only. If a concrete type does not implement the required trait(s), a semantic error is reported. Generic function/struct bodies are NOT checked against bounds at definition time.

**Phase B (planned):** Bodies will be checked against bounds at definition time, enabling safe method calls on bounded type parameters.

## The Universal `print()` Function

With traits, a universal `print()` function can be defined:

```roxy
// In prelude/builtin
trait Printable;
fun Printable.print();
fun Printable.println() {
    self.print();
    print("\n");
}

// Builtin implementations
fun i32.print() for Printable {
    print(f"{self}");
}

fun i64.print() for Printable {
    print(f"{self}");
}

fun f64.print() for Printable {
    print(f"{self}");
}

fun bool.print() for Printable {
    print(f"{self}");
}

fun string.print() for Printable {
    print(self);
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
// Built-in list implements Printable if element type does
fun List<T>.print() for Printable where T: Printable {
    print("[");
    for (var i: i32 = 0; i < self.len(); i = i + 1) {
        if (i > 0) { print(", "); }
        self[i].print();
    }
    print("]");
}

// Option<T> implements Printable if T does
fun Option.print<T>() for Printable where T: Printable {
    if (self.has_value()) {
        print("Some(");
        self.unwrap().print();
        print(")");
    } else {
        print("None");
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
    print("\n");
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
    print(self.name);
    print(": ");
    print(f"{self.value}");
}

fun Score.compare(other: Score): i32 for Comparable {
    return self.value - other.value;
}

// === Generic function ===
fun print_winner<T: Printable + Comparable>(a: T, b: T) {
    print("Winner: ");
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
trait_decl      -> "trait" Identifier generic_params? ( ":" Identifier )? ";" ;

trait_method    -> "fun" Identifier "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

impl_method     -> "fun" Identifier "." Identifier
                   "(" parameters? ")" ( ":" type_expr )?
                   "for" Identifier generic_args?
                   block ;

generic_params  -> "<" type_param ( "," type_param )* ">" ;
type_param      -> Identifier ( ":" trait_bounds )? ;
trait_bounds    -> trait_bound ( "+" trait_bound )* ;
trait_bound     -> Identifier generic_args? ;
generic_args    -> "<" type_expr ( "," type_expr )* ">" ;
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

Default methods (trait methods with bodies) are deep-cloned for each implementing struct that doesn't override them. A `TypeSubstitution` is built that maps `Self` → the concrete struct type, plus any trait type parameters → their concrete type arguments. The body, parameters, and return type are cloned using `GenericInstantiator::clone_stmt()` and `substitute_type_expr()`, which perform the substitution during cloning. These are added as synthetic declarations processed alongside regular methods.

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
| `include/roxy/compiler/types.hpp` | `TraitMethodInfo`, `TraitTypeInfo` (with `type_params`), trait type kind |
| `include/roxy/compiler/ast.hpp` | `TraitDecl` (with `type_params`), `MethodDecl` (with `trait_type_args`) |
| `src/roxy/compiler/parser.cpp` | `trait_declaration()`, `for Trait<Args>` parsing |
| `include/roxy/compiler/generics.hpp` | `clone_stmt()`, `substitute_type_expr()` (used by default method injection) |
| `src/roxy/compiler/semantic.cpp` | Trait analysis, validation, generic trait type arg resolution, default method injection |
| `src/roxy/compiler/ir_builder.cpp` | Operator dispatch, synthetic decl processing |
| `tests/e2e/test_traits.cpp` | E2E tests (11 non-generic + 6 generic trait tests) |
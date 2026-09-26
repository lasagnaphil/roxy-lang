# Traits

Traits define shared behavior across types, enabling ad-hoc polymorphism (free-function overloading also exists; see [overloading.md](overloading.md)). A trait is a named set of methods; types implement it with `for Trait` impls, and generic code constrains type parameters with trait bounds. Trait methods live on the struct and reuse the existing method machinery — there is no trait-object runtime.

Operator dispatch is covered in `operator-overloading.md`; trait bounds on generics in `generics.md`.

**Builtin traits** (usable without a user declaration): `Printable`, `Hash`, `Eq`, `Ord` (`lt`/`le`/`gt`/`ge`), `Exception`, and the subscript-operator traits `Index<Idx, Output>` / `IndexMut<Idx, Output>`. Other operator traits (`Add<Rhs>`, …) are user-declared.

**Not yet implemented:** a standard-library trait set (`Clone`, `Default`, `Iterator`, …) and generic trait inheritance (`trait AddAssign<Rhs> : Add<Rhs>`).

## Builtin trait membership on primitives

Primitives formally implement builtin traits (the `m_primitive_traits` table),
so `<T: Trait>` bounds instantiate at them:

| Trait | Primitive kinds |
|---|---|
| `Printable` (`to_string(): string`) | bool, i32, i64, u32, u64, f32, f64, string (+ enums via i32; + `List<T>`/`Map<K,V>` structurally when the element/key/value types are Printable) |
| `Hash` (`hash(): u64`) | all integer kinds, bool, f32/f64, string |
| `Eq` (`eq(other: Self): bool`) | every kind with eq/ne operator methods (all of the above) |
| `Ord` (`lt`/`le`/`gt`/`ge`) | i32, i64, u32, u64, f32, f64 (+ enums via i32, ordered by discriminant — `Color::Red < Color::Blue` is legal). NOT string (no ordered string ops) |

For `Eq`/`Ord` only trait *membership* is registered — the concrete
`eq`/`lt`/… methods come from `register_primitive_operator_methods` (Self-typed
duplicates would shadow the native operator dispatch). Explicit method calls on
primitive receivers work: `42.to_string()` / `x.hash()` call the native
(`i32$$to_string`, …), and operator-named calls like `a.lt(b)` lower to raw IR
ops (see `operator-overloading.md` § Unified Dispatch).

A user redeclaration of a builtin trait (`trait Ord : Eq;` with default
`le`/`gt`/`ge` bodies) merges with the builtin: the builtin *shape* stays, and
the user's default bodies are adopted onto the pre-registered entries, so
default-method injection works.

## Declaring Traits and Methods

Roxy uses free-floating syntax, consistent with struct methods. A method with **no body is required**; a method **with a body is a default** that implementers may override.

```roxy
trait Comparable;
fun Comparable.compare(other: Self): i32;            // required (no body)

// default methods, built on the required one
fun Comparable.lt(other: Self): bool { return self.compare(other) < 0; }
fun Comparable.gt(other: Self): bool { return self.compare(other) > 0; }
fun Comparable.eq(other: Self): bool { return self.compare(other) == 0; }
```

`Self` refers to the implementing type in signatures.

## Implementing Traits

The `for Trait` suffix implements a trait method for a type. Implementing the required methods gives the type all of the trait's default methods too.

```roxy
struct Point { x: i32; y: i32; }

fun Point.compare(other: Point): i32 for Comparable {
    return (self.x*self.x + self.y*self.y) - (other.x*other.x + other.y*other.y);
}
// Point now has compare(), lt(), gt(), eq() (defaults injected)

// a default can be overridden by providing a body for it
fun Point.eq(other: Point): bool for Comparable {
    return self.x == other.x && self.y == other.y;
}
```

If a required method is missing, the compiler reports an *incomplete trait implementation* error naming the missing method.

## Trait Inheritance

A trait can extend another; implementing the sub-trait requires also implementing the parent.

```roxy
trait Describe;
fun Describe.describe();

trait DebugDescribe : Describe;
fun DebugDescribe.debug_describe() {
    print("[DEBUG] ");
    self.describe();
}
```

## Generic Traits

Traits can take type parameters, enabling mixed-type operations. Implementations supply concrete type arguments in the `for` clause.

```roxy
trait Mul<Rhs>;
fun Mul.mul(other: Rhs): Self;

struct Vec2 { x: i32; y: i32; }

fun Vec2.mul(scalar: i32): Vec2 for Mul<i32> {     // Rhs = i32
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}
```

Default methods may use trait type parameters; when injected into a struct, parameters are substituted with the concrete type arguments (e.g. an `add_twice` default calling `self.add(other)` gets `Rhs` substituted per implementation).

**Constraints:**
- A struct implements a given generic trait at most once. Methods are one-per-name, so a second instantiation (`for Mul<i32>` plus `for Mul<V>`) would define a second `mul` and is rejected as a duplicate method.
- A bare `for Mul` on a generic trait means `Rhs = Self` (`fun V.mul(o: V): V for Mul`); write `for Mul<i32>` for any other argument.
- The compiler rejects type args on a non-generic trait (`for Eq<i32>`).
- Generic trait inheritance (`trait AddAssign<Rhs> : Add<Rhs>`) is not yet supported.

## Trait Bounds

Type parameters can be constrained with trait bounds. Bounds are checked at every instantiation site, and the bodies of bounded generics are checked against their declared bounds at definition time (catching nonexistent method calls without needing a concrete instantiation). See `generics.md` for the full treatment.

```roxy
fun identity_both<T: Printable + Hash>(value: T): T { return value; }
struct HashBox<T: Hash> { value: T; }
```

## `Printable` and `print()`

`print` accepts any `Printable` value: primitives hit a native overload, and anything else implementing `Printable` (structs with a `for Printable` impl, enums, printable containers) is rewritten to `print(value.to_string())` — see [overloading.md](overloading.md). Interpolation (`f"{v}"`), explicit `v.to_string()`, and `print(v)` all share the same `Printable` query (`type_implements_printable`), which also consults active trait bounds inside bounded generic bodies — `fun show<T: Printable>(v: T) { print(f"{v}"); }` type-checks at definition time.

## Method Resolution and Name Mangling

When a method is called on a type:

1. Use the type's direct implementation (`fun Type.method() for Trait`) if present.
2. Otherwise fall back to the trait's default, deep-cloned and injected into the struct.
3. Error if a required method has neither.

Default methods are injected by deep-cloning the trait method with `Self` → the concrete struct and each trait type parameter → its concrete argument; the clones are processed as synthetic declarations alongside regular methods.

Trait methods are stored on the struct and use standard method mangling, `Type$$method` (e.g. `Point$$eq`). Each struct has at most one implementation per method name: a trait impl whose name is already taken — by a plain method or by another trait's impl — is a `duplicate method` error, the same as two plain methods.

## Grammar

```
trait_decl   -> "trait" Identifier generic_params? ( ":" Identifier )? ";" ;
trait_method -> "fun" Identifier "." Identifier "(" parameters? ")"
                ( ":" type_expr )? ( block | ";" ) ;
impl_method  -> "fun" Identifier "." Identifier "(" parameters? ")"
                ( ":" type_expr )? "for" Identifier generic_args? block ;

generic_params -> "<" type_param ( "," type_param )* ">" ;
type_param     -> Identifier ( ":" trait_bounds )? ;
trait_bounds   -> trait_bound ( "+" trait_bound )* ;
trait_bound    -> Identifier generic_args? ;
generic_args   -> "<" type_expr ( "," type_expr )* ">" ;
```

## Files

Trait analysis, validation, and default-method injection: `src/roxy/compiler/sema/trait_system.cpp` (`TraitSystem`). Tests: `tests/e2e/test_traits.cpp`.

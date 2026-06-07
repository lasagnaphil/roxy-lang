# Operator Overloading

Operators in Roxy are implemented via traits. The compiler rewrites each operator into a trait method call, so user-defined types support standard operators through the same dispatch path as primitives.

Dispatch covers arithmetic (`+ - * / %`), comparison (`== != < <= > >=`), bitwise (`& | ^ ~ << >>`), unary (`-`, `~`), compound assignment (`+=` etc.), and indexing (`[]`), for both struct and primitive types, through a unified trait-method lookup. Lists participate via `index`/`index_mut` methods registered alongside their native methods.

See `traits.md` for the general trait system design.

## Operator Traits

The operator traits are builtin trait declarations (no user-visible source; declared in semantic analysis). Each maps an operator to a method name. `Rhs` defaults to `Self` on the binary arithmetic/bitwise traits.

### Comparison

| Trait | Methods | Operators |
|-------|---------|-----------|
| `Eq` | `eq`, `ne` | `==`, `!=` |
| `Ord` : `Eq` | `cmp`, `lt`, `le`, `gt`, `ge` | `<`, `<=`, `>`, `>=` |

`ne` defaults to `!eq`; the `Ord` comparisons default to `cmp(other)` (which returns -1/0/1) against 0.

### Arithmetic

| Trait | Method | Operator |
|-------|--------|----------|
| `Add<Rhs>` | `add` | `+` |
| `Sub<Rhs>` | `sub` | `-` |
| `Mul<Rhs>` | `mul` | `*` |
| `Div<Rhs>` | `div` | `/` |
| `Mod<Rhs>` | `mod` | `%` |
| `Neg` | `neg` | `-x` (unary) |

### Compound Assignment

These modify `self` in-place (return `void`).

| Trait | Method | Operator |
|-------|--------|----------|
| `AddAssign<Rhs>` | `add_assign` | `+=` |
| `SubAssign<Rhs>` | `sub_assign` | `-=` |
| `MulAssign<Rhs>` | `mul_assign` | `*=` |
| `DivAssign<Rhs>` | `div_assign` | `/=` |
| `ModAssign<Rhs>` | `mod_assign` | `%=` |
| `BitAndAssign<Rhs>` | `bit_and_assign` | `&=` |
| `BitOrAssign<Rhs>` | `bit_or_assign` | `\|=` |
| `BitXorAssign<Rhs>` | `bit_xor_assign` | `^=` |
| `ShlAssign<Rhs>` | `shl_assign` | `<<=` |
| `ShrAssign<Rhs>` | `shr_assign` | `>>=` |

### Bitwise

| Trait | Method | Operator |
|-------|--------|----------|
| `BitAnd<Rhs>` | `bit_and` | `&` |
| `BitOr<Rhs>` | `bit_or` | `\|` |
| `BitXor<Rhs>` | `bit_xor` | `^` |
| `BitNot` | `bit_not` | `~` (unary) |
| `Shl<Rhs>` | `shl` | `<<` |
| `Shr<Rhs>` | `shr` | `>>` |

### Indexing

| Trait | Method | Operator | Access |
|-------|--------|----------|--------|
| `Index<Idx>` | `index` | `a[i]` | read |
| `IndexMut<Idx>` | `index_mut` | `a[i] = v` | write |

### Non-Overloadable Operators

| Operators | Reason |
|-----------|--------|
| `&&`, `\|\|` | Short-circuit evaluation semantics |
| `!` | Reserved for boolean only |
| `=` | Assignment is not an expression |
| `.` | Member access |
| `::` | Scope resolution |

## Compiler Rewrites

Each operator is rewritten to its trait method call (binary trait `Rhs` shown as `_`, resolved by lookup):

| Operator | Rewrite | Operator | Rewrite |
|----------|---------|----------|---------|
| `a + b` | `a.add(b)` | `a += b` | `a.add_assign(b)` |
| `a - b` | `a.sub(b)` | `a -= b` | `a.sub_assign(b)` |
| `a * b` | `a.mul(b)` | `a *= b` | `a.mul_assign(b)` |
| `a / b` | `a.div(b)` | `a /= b` | `a.div_assign(b)` |
| `a % b` | `a.mod(b)` | `a %= b` | `a.mod_assign(b)` |
| `-a` | `a.neg()` | | |
| `a == b` | `a.eq(b)` | `a != b` | `a.ne(b)` |
| `a < b` | `a.lt(b)` | `a <= b` | `a.le(b)` |
| `a > b` | `a.gt(b)` | `a >= b` | `a.ge(b)` |
| `a & b` | `a.bit_and(b)` | `a &= b` | `a.bit_and_assign(b)` |
| `a \| b` | `a.bit_or(b)` | `a \|= b` | `a.bit_or_assign(b)` |
| `a ^ b` | `a.bit_xor(b)` | `a ^= b` | `a.bit_xor_assign(b)` |
| `a << b` | `a.shl(b)` | `a <<= b` | `a.shl_assign(b)` |
| `a >> b` | `a.shr(b)` | `a >>= b` | `a.shr_assign(b)` |
| `~a` | `a.bit_not()` | | |
| `a[i]` | `a.index(i)` | `a[i] = v` | `a.index_mut(i, v)` |

## Example

A struct opts into an operator by implementing the trait method with a `for Trait` clause. Same-type operations default `Rhs` to `Self`; mixed-type operations name the right-hand type explicitly.

```roxy
struct Vec2 { x: f64; y: f64; }

fun Vec2.add(other: Vec2): Vec2 for Add {        // Rhs = Self
    return Vec2 { x = self.x + other.x, y = self.y + other.y };
}

fun Vec2.mul(scalar: f64): Vec2 for Mul<f64> {   // mixed-type, requires generics
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}

fun Vec2.add_assign(other: Vec2) for AddAssign { // in-place
    self.x = self.x + other.x;
    self.y = self.y + other.y;
}

fun Vec2.eq(other: Vec2): bool for Eq {
    return self.x == other.x && self.y == other.y;
}

fun main() {
    var a = Vec2 { x = 1.0, y = 2.0 };
    var b = Vec2 { x = 3.0, y = 4.0 };
    var c = a + b;     // a.add(b)
    var d = a * 2.0;   // a.mul(2.0)
    a += b;            // a.add_assign(b)
    if (a == b) { /* ... */ }
}
```

Mixed-type operator traits (`Mul<f64>`, `Add<i32>`) rely on generic traits — see `traits.md` § Generic Traits.

## Unified Dispatch

Both primitive and struct operators resolve through one path, but codegen diverges.

- **Registration (Pass 1.8):** `register_primitive_operator_methods()` registers operator methods on primitive types via `TypeCache::register_primitive_method()`; `populate_list_methods()` registers `index`/`index_mut` on list types. Primitive methods are not user-writable.
- **Resolution:** `try_resolve_binary_op()`, `try_resolve_unary_op()`, and `analyze_index_expr()` call `TypeCache::lookup_method()`, which dispatches to struct hierarchy, primitive, or list lookup. Type checking is uniform across all kinds.
- **Code generation:** primitives emit **direct IR ops** (`AddI`, `SubF`, …) rather than method calls; structs emit trait-method calls; lists/maps emit `CallNative` to their registered `index`/`index_mut` functions.

Which primitive types carry which operators:

| Type | Arithmetic | Bitwise | Comparison | Unary | Compound assign |
|------|-----------|---------|-----------|-------|-----------------|
| `i32`, `i64` | `add sub mul div mod` | `bit_and bit_or bit_xor shl shr` | all six | `neg bit_not` | all integer forms |
| `f32`, `f64` | `add sub mul div` | — | all six | `neg` | `add/sub/mul/div_assign` |
| `bool` | — | — | `eq ne` | — | — |
| `List<T>` | — | — | — | — | `index` / `index_mut` |

The shared operator→method-name mappings live in `include/roxy/compiler/operator_traits.hpp` (`binary_op_to_trait_method()`, `unary_op_to_trait_method()`, `assign_op_to_trait_method()`), used by both semantic analysis and IR generation.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/operator_traits.hpp` | operator → method-name mappings |
| `src/roxy/compiler/semantic.cpp` | trait/primitive registration, operator resolution |
| `src/roxy/compiler/ir_builder.cpp` | direct IR ops for primitives, trait calls for structs |
| `tests/e2e/test_traits.cpp` | operator-overloading E2E tests |

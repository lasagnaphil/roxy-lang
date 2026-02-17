# Operator Overloading

Operators in Roxy are implemented via traits. The compiler rewrites operators into trait method calls, enabling user-defined types to support standard operators.

**Implemented:** Comparison operator dispatch (`==`, `!=`, `<`, `<=`, `>`, `>=`) for struct types with `eq`/`ne`/`lt`/`le`/`gt`/`ge` methods.

**Not yet implemented:** Arithmetic, bitwise, compound assignment, and indexing operator dispatch. Generic traits (the foundation for mixed-type `Rhs`) are now implemented — what remains is wiring the operator-to-method dispatch in semantic analysis and IR generation.

See `traits.md` for the general trait system design.

## Operator Traits

### Comparison Traits

| Trait | Methods | Operators |
|-------|---------|-----------|
| `Eq` | `eq`, `ne` | `==`, `!=` |
| `Ord` : `Eq` | `cmp`, `lt`, `le`, `gt`, `ge` | `<`, `<=`, `>`, `>=` |

### Arithmetic Traits

| Trait | Methods | Operators | Notes |
|-------|---------|-----------|-------|
| `Add<Rhs>` | `add` | `+` | `Rhs` defaults to `Self` |
| `Sub<Rhs>` | `sub` | `-` | `Rhs` defaults to `Self` |
| `Mul<Rhs>` | `mul` | `*` | `Rhs` defaults to `Self` |
| `Div<Rhs>` | `div` | `/` | `Rhs` defaults to `Self` |
| `Mod<Rhs>` | `mod` | `%` | `Rhs` defaults to `Self` |
| `Neg` | `neg` | `-x` | Unary negation |

### Compound Assignment Traits

| Trait | Methods | Operators | Notes |
|-------|---------|-----------|-------|
| `AddAssign<Rhs>` | `add_assign` | `+=` | Modifies `self` in-place |
| `SubAssign<Rhs>` | `sub_assign` | `-=` | Modifies `self` in-place |
| `MulAssign<Rhs>` | `mul_assign` | `*=` | Modifies `self` in-place |
| `DivAssign<Rhs>` | `div_assign` | `/=` | Modifies `self` in-place |
| `ModAssign<Rhs>` | `mod_assign` | `%=` | Modifies `self` in-place |

### Bitwise Traits

| Trait | Methods | Operators | Notes |
|-------|---------|-----------|-------|
| `BitAnd<Rhs>` | `bit_and` | `&` | `Rhs` defaults to `Self` |
| `BitOr<Rhs>` | `bit_or` | `\|` | `Rhs` defaults to `Self` |
| `BitXor<Rhs>` | `bit_xor` | `^` | `Rhs` defaults to `Self` |
| `BitNot` | `bit_not` | `~` | Unary bitwise NOT |
| `Shl<Rhs>` | `shl` | `<<` | Shift left |
| `Shr<Rhs>` | `shr` | `>>` | Shift right |

### Bitwise Assignment Traits

| Trait | Methods | Operators |
|-------|---------|-----------|
| `BitAndAssign<Rhs>` | `bit_and_assign` | `&=` |
| `BitOrAssign<Rhs>` | `bit_or_assign` | `\|=` |
| `BitXorAssign<Rhs>` | `bit_xor_assign` | `^=` |
| `ShlAssign<Rhs>` | `shl_assign` | `<<=` |
| `ShrAssign<Rhs>` | `shr_assign` | `>>=` |

### Indexing Traits

| Trait | Methods | Operators | Notes |
|-------|---------|-----------|-------|
| `Index<Idx>` | `index` | `a[i]` | Read access |
| `IndexMut<Idx>` | `index_mut` | `a[i] = v` | Write access |

### Non-Overloadable Operators

These operators are **not** overloadable:

| Operators | Reason |
|-----------|--------|
| `&&`, `\|\|` | Short-circuit evaluation semantics |
| `!` | Reserved for boolean only |
| `=` | Assignment is not an expression |
| `.` | Member access |
| `::` | Scope resolution |

## Trait Definitions

```roxy
// === Comparison ===
trait Eq;
fun Eq.eq(other: Self): bool;
fun Eq.ne(other: Self): bool { return !self.eq(other); }

trait Ord : Eq;
fun Ord.cmp(other: Self): i32;  // -1, 0, or 1
fun Ord.lt(other: Self): bool { return self.cmp(other) < 0; }
fun Ord.le(other: Self): bool { return self.cmp(other) <= 0; }
fun Ord.gt(other: Self): bool { return self.cmp(other) > 0; }
fun Ord.ge(other: Self): bool { return self.cmp(other) >= 0; }

// === Arithmetic ===
trait Add<Rhs>;      // Rhs defaults to Self
fun Add.add(other: Rhs): Self;

trait Sub<Rhs>;
fun Sub.sub(other: Rhs): Self;

trait Mul<Rhs>;
fun Mul.mul(other: Rhs): Self;

trait Div<Rhs>;
fun Div.div(other: Rhs): Self;

trait Mod<Rhs>;
fun Mod.mod(other: Rhs): Self;

trait Neg;
fun Neg.neg(): Self;

// === Compound Assignment ===
trait AddAssign<Rhs>;
fun AddAssign.add_assign(other: Rhs);  // modifies self in-place

trait SubAssign<Rhs>;
fun SubAssign.sub_assign(other: Rhs);

trait MulAssign<Rhs>;
fun MulAssign.mul_assign(other: Rhs);

trait DivAssign<Rhs>;
fun DivAssign.div_assign(other: Rhs);

trait ModAssign<Rhs>;
fun ModAssign.mod_assign(other: Rhs);

// === Bitwise ===
trait BitAnd<Rhs>;
fun BitAnd.bit_and(other: Rhs): Self;

trait BitOr<Rhs>;
fun BitOr.bit_or(other: Rhs): Self;

trait BitXor<Rhs>;
fun BitXor.bit_xor(other: Rhs): Self;

trait BitNot;
fun BitNot.bit_not(): Self;

trait Shl<Rhs>;
fun Shl.shl(other: Rhs): Self;

trait Shr<Rhs>;
fun Shr.shr(other: Rhs): Self;

// === Bitwise Assignment ===
trait BitAndAssign<Rhs>;
fun BitAndAssign.bit_and_assign(other: Rhs);

trait BitOrAssign<Rhs>;
fun BitOrAssign.bit_or_assign(other: Rhs);

trait BitXorAssign<Rhs>;
fun BitXorAssign.bit_xor_assign(other: Rhs);

trait ShlAssign<Rhs>;
fun ShlAssign.shl_assign(other: Rhs);

trait ShrAssign<Rhs>;
fun ShrAssign.shr_assign(other: Rhs);

// === Indexing ===
trait Index<Idx>;
fun Index.index(i: Idx): Self;

trait IndexMut<Idx>;
fun IndexMut.index_mut(i: Idx, value: Self);
```

## Compiler Rewrites

The compiler transforms operators into trait method calls:

**Arithmetic:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a + b` | `a.add(b)` | `Add<_>` |
| `a - b` | `a.sub(b)` | `Sub<_>` |
| `a * b` | `a.mul(b)` | `Mul<_>` |
| `a / b` | `a.div(b)` | `Div<_>` |
| `a % b` | `a.mod(b)` | `Mod<_>` |
| `-a` | `a.neg()` | `Neg` |

**Compound Assignment:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a += b` | `a.add_assign(b)` | `AddAssign<_>` |
| `a -= b` | `a.sub_assign(b)` | `SubAssign<_>` |
| `a *= b` | `a.mul_assign(b)` | `MulAssign<_>` |
| `a /= b` | `a.div_assign(b)` | `DivAssign<_>` |
| `a %= b` | `a.mod_assign(b)` | `ModAssign<_>` |

**Comparison:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a == b` | `a.eq(b)` | `Eq` |
| `a != b` | `a.ne(b)` | `Eq` |
| `a < b` | `a.lt(b)` | `Ord` |
| `a <= b` | `a.le(b)` | `Ord` |
| `a > b` | `a.gt(b)` | `Ord` |
| `a >= b` | `a.ge(b)` | `Ord` |

**Bitwise:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a & b` | `a.bit_and(b)` | `BitAnd<_>` |
| `a \| b` | `a.bit_or(b)` | `BitOr<_>` |
| `a ^ b` | `a.bit_xor(b)` | `BitXor<_>` |
| `~a` | `a.bit_not()` | `BitNot` |
| `a << b` | `a.shl(b)` | `Shl<_>` |
| `a >> b` | `a.shr(b)` | `Shr<_>` |

**Bitwise Assignment:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a &= b` | `a.bit_and_assign(b)` | `BitAndAssign<_>` |
| `a \|= b` | `a.bit_or_assign(b)` | `BitOrAssign<_>` |
| `a ^= b` | `a.bit_xor_assign(b)` | `BitXorAssign<_>` |
| `a <<= b` | `a.shl_assign(b)` | `ShlAssign<_>` |
| `a >>= b` | `a.shr_assign(b)` | `ShrAssign<_>` |

**Indexing:**

| Operator | Rewritten To | Trait |
|----------|--------------|-------|
| `a[i]` (read) | `a.index(i)` | `Index<_>` |
| `a[i] = v` (write) | `a.index_mut(i, v)` | `IndexMut<_>` |

## Example: Vector Math

```roxy
struct Vec2 { x: f64; y: f64; }

// Same-type operations (no generics needed)
fun Vec2.add(other: Vec2): Vec2 for Add {
    return Vec2 { x = self.x + other.x, y = self.y + other.y };
}

fun Vec2.sub(other: Vec2): Vec2 for Sub {
    return Vec2 { x = self.x - other.x, y = self.y - other.y };
}

fun Vec2.neg(): Vec2 for Neg {
    return Vec2 { x = -self.x, y = -self.y };
}

fun Vec2.eq(other: Vec2): bool for Eq {
    return self.x == other.x && self.y == other.y;
}

// In-place operations
fun Vec2.add_assign(other: Vec2) for AddAssign {
    self.x = self.x + other.x;
    self.y = self.y + other.y;
}

// Mixed-type operations (requires generics)
fun Vec2.mul(scalar: f64): Vec2 for Mul<f64> {
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}

fun Vec2.div(scalar: f64): Vec2 for Div<f64> {
    return Vec2 { x = self.x / scalar, y = self.y / scalar };
}

fun Vec2.mul_assign(scalar: f64) for MulAssign<f64> {
    self.x = self.x * scalar;
    self.y = self.y * scalar;
}

// Usage
fun main(): i32 {
    var a: Vec2 = Vec2 { x = 1.0, y = 2.0 };
    var b: Vec2 = Vec2 { x = 3.0, y = 4.0 };

    var c: Vec2 = a + b;      // Vec2.add(b)
    var d: Vec2 = a - b;      // Vec2.sub(b)
    var e: Vec2 = -a;         // Vec2.neg()
    var f: Vec2 = a * 2.0;    // Vec2.mul(2.0) - requires generics
    var g: Vec2 = a / 2.0;    // Vec2.div(2.0) - requires generics

    a += b;                   // Vec2.add_assign(b)
    a *= 2.0;                 // Vec2.mul_assign(2.0) - requires generics

    if (a == b) { /* ... */ }
    if (a != b) { /* ... */ }

    return 0;
}
```

## Builtin Implementations

Primitive types have compiler-provided trait implementations:

```roxy
// Compiler intrinsics (not user-writable)
fun i32.add(other: i32): i32 for Add { /* intrinsic */ }
fun i32.sub(other: i32): i32 for Sub { /* intrinsic */ }
fun i32.mul(other: i32): i32 for Mul { /* intrinsic */ }
fun i32.div(other: i32): i32 for Div { /* intrinsic */ }
fun i32.mod(other: i32): i32 for Mod { /* intrinsic */ }
fun i32.neg(): i32 for Neg { /* intrinsic */ }

fun i32.add_assign(other: i32) for AddAssign { /* intrinsic */ }
fun i32.sub_assign(other: i32) for SubAssign { /* intrinsic */ }
// ... etc

fun i32.eq(other: i32): bool for Eq { /* intrinsic */ }
fun i32.cmp(other: i32): i32 for Ord { /* intrinsic */ }

fun i32.bit_and(other: i32): i32 for BitAnd { /* intrinsic */ }
fun i32.bit_or(other: i32): i32 for BitOr { /* intrinsic */ }
fun i32.bit_xor(other: i32): i32 for BitXor { /* intrinsic */ }
fun i32.bit_not(): i32 for BitNot { /* intrinsic */ }
fun i32.shl(other: i32): i32 for Shl { /* intrinsic */ }
fun i32.shr(other: i32): i32 for Shr { /* intrinsic */ }
// ... etc for all primitive types
```

## Generics Requirement

Generic traits with type parameters are now implemented (see `traits.md` § Generic Traits). This means both same-type and mixed-type operator traits can be declared and implemented:

```roxy
// Same-type: Rhs = Self
fun Vec2.add(other: Vec2): Vec2 for Add<Vec2> { ... }

// Mixed-type: Rhs = i32
fun Vec2.mul(scalar: i32): Vec2 for Mul<i32> { ... }
```

**What remains** is wiring operator dispatch: the compiler needs to rewrite `a + b` → `a.add(b)` and `a * 2` → `a.mul(2)` for arithmetic/bitwise operators (comparison operators are already dispatched).
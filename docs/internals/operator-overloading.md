# Operator Overloading

Operators in Roxy are implemented via traits. The compiler rewrites each operator into a method call, so user-defined types support standard operators through the same dispatch path as primitives. See `traits.md` for the general trait system.

## Operator Traits

Operator dispatch is **structural**: the compiler rewrites each operator to a method call (`a + b` → `a.add(b)`, `a[i]` → `a.index(i)`) and resolves it by method name, so a type opts into an operator simply by defining the method — the `for Trait` clause is optional bookkeeping (it validates the signature and injects defaults). `Rhs` defaults to `Self` on the binary arithmetic/bitwise traits.

Most operator traits below (`Add<Rhs>`, `Mul<Rhs>`, …) are **not** builtin — user code declares them (`trait Add<Rhs>;`) when it wants the `for` clause. The builtin ones (usable in `for ...` without a declaration) are `Eq`, `Ord`, and `Index` / `IndexMut`; see `traits.md` for the full builtin list.

| Trait | Method | Operator | | Trait | Method | Operator |
|-------|--------|----------|-|-------|--------|----------|
| `Add<Rhs>` | `add` | `+` | | `AddAssign<Rhs>` | `add_assign` | `+=` |
| `Sub<Rhs>` | `sub` | `-` | | `SubAssign<Rhs>` | `sub_assign` | `-=` |
| `Mul<Rhs>` | `mul` | `*` | | `MulAssign<Rhs>` | `mul_assign` | `*=` |
| `Div<Rhs>` | `div` | `/` | | `DivAssign<Rhs>` | `div_assign` | `/=` |
| `Mod<Rhs>` | `mod` | `%` | | `ModAssign<Rhs>` | `mod_assign` | `%=` |
| `BitAnd<Rhs>` | `bit_and` | `&` | | `BitAndAssign<Rhs>` | `bit_and_assign` | `&=` |
| `BitOr<Rhs>` | `bit_or` | `\|` | | `BitOrAssign<Rhs>` | `bit_or_assign` | `\|=` |
| `BitXor<Rhs>` | `bit_xor` | `^` | | `BitXorAssign<Rhs>` | `bit_xor_assign` | `^=` |
| `Shl<Rhs>` | `shl` | `<<` | | `ShlAssign<Rhs>` | `shl_assign` | `<<=` |
| `Shr<Rhs>` | `shr` | `>>` | | `ShrAssign<Rhs>` | `shr_assign` | `>>=` |
| `Neg` | `neg` | `-x` | | `BitNot` | `bit_not` | `~x` |
| `Eq` | `eq` | `==` | | — | `ne` | `!=` |
| `Ord` | `lt`, `le`, `gt`, `ge` | `<`, `<=`, `>`, `>=` | | | | |
| `Index<Idx, Output>` | `index` | `a[i]` (read) | | `IndexMut<Idx, Output>` | `index_mut` | `a[i] = v` (write) |

Compound-assignment methods modify `self` in place and return `void`.

**Comparisons have no defaults between them:** each operator dispatches to its own method, so a struct that defines only `eq` supports `==` but not `!=` (`invalid operands for comparison operator`) — `!=` needs its own `ne` method, which belongs to no builtin trait — and a `for Ord` impl must define all four comparisons (there is no `cmp`).

**`Index` / `IndexMut` carry two type parameters** — the index type `Idx` and the element type `Output` — because Roxy has no associated types to name the element type the way Rust's `Index { type Output; }` does. So `index(idx: Idx): Output` and `index_mut(idx: Idx, val: Output)`; an impl writes `for Index<i32, uniq Cell>`. The `for` clause validates the index/element types against the method signature when present. `List<T>` / `Map<K, V>` provide `index` / `index_mut` as native methods rather than via the trait.

### Non-Overloadable Operators

| Operators | Reason |
|-----------|--------|
| `&&`, `\|\|` | Short-circuit evaluation semantics |
| `!` | Reserved for boolean only |
| `=` | Assignment is not an expression |
| `.` | Member access |
| `::` | Scope resolution |

### The receiver may be an rvalue

`self` is passed as a pointer, so the receiver needs an address. Operator dispatch takes it with `gen_lvalue_addr(expr, rvalue_ok=true)`, whose fallback for a non-place expression is `gen_expr`; no materialization is needed because lowering already unpacks a struct return into a stack-allocated pointer. This is what lets operators chain — `(a + b) * 2` uses one operator's result as the next one's receiver.

## Example

```roxy
struct Vec2 { x: f64; y: f64; }

fun Vec2.add(other: Vec2): Vec2 for Add {        // Rhs = Self
    return Vec2 { x = self.x + other.x, y = self.y + other.y };
}

fun Vec2.mul(scalar: f64): Vec2 for Mul<f64> {   // mixed-type (generic trait)
    return Vec2 { x = self.x * scalar, y = self.y * scalar };
}

fun Vec2.add_assign(other: Vec2) for AddAssign { // in-place
    self.x = self.x + other.x;
    self.y = self.y + other.y;
}

fun main() {
    var a = Vec2 { x = 1.0, y = 2.0 };
    var b = Vec2 { x = 3.0, y = 4.0 };
    var c = a + b;     // a.add(b)
    var d = a * 2.0;   // a.mul(2.0)
    a += b;            // a.add_assign(b)
}
```

Mixed-type operator traits (`Mul<f64>`, `Add<i32>`) rely on generic traits — see `traits.md` § Generic Traits.

## Unified Dispatch

Primitive, struct, and list operators all resolve through `TypeCache::lookup_method()` (operator methods are registered on primitive types by the trait system; they are not user-writable), so type checking is uniform. Codegen diverges:

- **primitives** emit **direct IR ops** (`AddI`, `SubF`, …), not calls;
- **structs** emit trait-method calls;
- **list/map subscripts** emit the dedicated `IROp::IndexGet` / `IndexSet` / `IndexTryAddr` ops (the registered `index`/`index_mut` natives only type them).

Which primitive types carry which operators:

| Type | Arithmetic | Bitwise | Comparison | Unary | Compound assign |
|------|-----------|---------|-----------|-------|-----------------|
| `i32`, `i64`, `u32`, `u64` | `add sub mul div mod` | `bit_and bit_or bit_xor shl shr` | all six | `neg bit_not` | all integer forms |
| `f32`, `f64` | `add sub mul div` | — | all six | `neg` | `add/sub/mul/div_assign` |
| `bool` | — | — | `eq ne` | — | — |

The operator↔method-name mappings live in `include/roxy/compiler/support/operator_traits.hpp`, shared by sema and IR generation. The reverse maps power **explicit operator-named method calls on primitive receivers**: `a.lt(b)` / `(10).add(5)` lowers to the same raw IR op the operator expression would emit (so `u64.lt` is `LtU`, `f64.add` is `AddD`), `"a".eq(b)` routes to the string natives, and enums compare as i32 discriminants. Compound-assign methods have no reverse mapping and are rejected as explicit calls (they need an assignable receiver). This is what makes generic bodies calling bound trait methods (`<T: Ord>` with `a.lt(b)`) work when instantiated at primitives.

**Tests:** `tests/e2e/test_traits.cpp`

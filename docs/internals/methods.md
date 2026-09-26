# Methods

Methods are functions associated with a struct type. They are declared outside the struct body with external `fun StructName.method()` syntax and carry an implicit `self` parameter. Methods compile to ordinary functions with mangled names — there is no method-specific runtime machinery.

## Syntax

```roxy
struct Point {
    x: i32;
    y: i32;
}

fun Point.sum(): i32 {
    return self.x + self.y;
}

fun Point.translate(dx: i32, dy: i32) {   // mutates self
    self.x = self.x + dx;
    self.y = self.y + dy;
}

fun Point.scaled(factor: i32): Point {    // returns a struct
    return Point { x = self.x * factor, y = self.y * factor };
}
```

## Method Calls

Methods are called with dot notation on a struct instance; the compiler passes the receiver as `self`:

```roxy
var p: Point = Point { x = 10, y = 20 };
print(p.sum());        // 30
p.translate(1, 2);
print(p.x);            // 11
var q: Point = p.scaled(2);
```

## Implicit `self`

Every method has an implicit first parameter `self` of type `ref<StructType>`. The logical signature of `Point.sum()` is `fun Point$$sum(self: ref<Point>): i32`. When calling `p.sum()`, the compiler passes `p` as the first argument automatically.

## Name Mangling

Methods compile to regular functions named `Struct$$method` (e.g. `Point$$sum`), with `self` as the first IR parameter; lowering treats them as any other function.

## Method vs. constructor disambiguation

Both method calls (`obj.method()`) and named-constructor calls (`Type.ctor()`) use `GetExpr` as the callee. The compiler distinguishes them by the receiver: if `GetExpr.object` is an identifier that resolves to a named type, it is a constructor call; otherwise it is a method call.

## Notes

- **Heap-allocated structs** — methods work identically on `uniq` receivers; `self` receives a reference to the heap object and mutations persist.
- **Visibility** — methods may be marked `pub` for export; non-public methods are module-private.
- **Inheritance** — methods *are* inherited and can be overridden, and `super` dispatches to the parent's implementation. See [inheritance.md](inheritance.md).
- **Non-struct receivers** — method-call *syntax* also works on primitives and enums (`42.to_string()`, `x.hash()`, `a.lt(b)`), but those resolve to natives or raw IR ops rather than user methods. See [traits.md](traits.md) and [operator-overloading.md](operator-overloading.md).
- **Coroutine methods** — a method whose body yields returns `Coro<T>`; `self` is captured into the state struct like any `ref` param. Supported on non-generic structs only. See [coroutines.md](coroutines.md).

## Limitations

- **No method overloading** — each struct can have only one method with a given name. Overloading exists for free functions and natives only; see [overloading.md](overloading.md).

**Tests:** `tests/e2e/test_methods.cpp`

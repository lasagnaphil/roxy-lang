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

Methods compile to regular functions with names mangled using the `$$` separator:

| Method Declaration | Mangled Name |
|--------------------|--------------|
| `fun Point.sum()` | `Point$$sum` |
| `fun Point.add(dx, dy)` | `Point$$add` |
| `fun Counter.increment()` | `Counter$$increment` |

## How It Works

The parser detects the `fun Identifier.` pattern and produces a `DeclMethod` AST node (`MethodDecl`, holding the struct name, method name, user-declared params — `self` excluded — return type, body, and visibility). Semantic analysis registers a `MethodInfo` on the struct's `StructTypeInfo.methods` (its `param_types` likewise excludes `self`) and type-checks the body with `self` in scope. The IR builder emits a function with the mangled name and `self` as the first parameter; lowering treats it as any other function. See `ast.hpp` (`MethodDecl`) and `types.hpp` (`MethodInfo`) for the exact field layouts.

### Method vs. constructor disambiguation

Both method calls (`obj.method()`) and named-constructor calls (`Type.ctor()`) use `GetExpr` as the callee. The compiler distinguishes them by the receiver: if `GetExpr.object` is an identifier that resolves to a named type, it is a constructor call; otherwise it is a method call.

## Notes

- **Heap-allocated structs** — methods work identically on `uniq` receivers; `self` receives a reference to the heap object and mutations persist.
- **Visibility** — methods may be marked `pub` for export; non-public methods are module-private.
- **Inheritance** — methods *are* inherited and can be overridden, and `super` dispatches to the parent's implementation. See [inheritance.md](inheritance.md).

## Limitations

- **No method overloading** — each struct can have only one method with a given name.

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/types.hpp` | `MethodInfo` struct |
| `include/roxy/compiler/ast.hpp` | `DeclMethod`, `MethodDecl` |
| `src/roxy/compiler/parser.cpp` | method parsing |
| `src/roxy/compiler/semantic.cpp` | method registration and body analysis |
| `src/roxy/compiler/ir_builder.cpp` | method IR generation (mangled name, `self` param) |
| `tests/e2e/test_methods.cpp` | E2E tests |

# Methods

Methods in Roxy are functions associated with struct types. They use external declaration syntax and have an implicit `self` parameter.

## Syntax

Methods are declared outside the struct body using `fun StructName.method_name()` syntax:

```roxy
struct Point {
    x: i32;
    y: i32;
}

// Method with no parameters (besides self)
fun Point.sum(): i32 {
    return self.x + self.y;
}

// Method with parameters
fun Point.add(dx: i32, dy: i32): i32 {
    return self.x + dx + self.y + dy;
}

// Method that modifies self
fun Point.translate(dx: i32, dy: i32) {
    self.x = self.x + dx;
    self.y = self.y + dy;
}

// Method returning a struct
fun Point.scaled(factor: i32): Point {
    return Point { x = self.x * factor, y = self.y * factor };
}
```

## Method Calls

Methods are called using dot notation on struct instances:

```roxy
fun main(): i32 {
    var p: Point = Point { x = 10, y = 20 };

    print(p.sum());           // Prints 30
    print(p.add(5, 15));      // Prints 50

    p.translate(1, 2);
    print(p.x);               // Prints 11

    var q: Point = p.scaled(2);
    print(q.x);               // Prints 22

    return 0;
}
```

## Implementation Details

### Name Mangling

Methods are compiled as regular functions with mangled names using the `$` separator:

| Method Declaration | Mangled Name |
|-------------------|--------------|
| `fun Point.sum()` | `Point$sum` |
| `fun Point.add(dx, dy)` | `Point$add` |
| `fun Counter.increment()` | `Counter$increment` |

### Implicit `self` Parameter

Every method has an implicit first parameter named `self` with type `ref<StructType>`:

```
// Logical signature of Point.sum():
fun Point$sum(self: ref<Point>): i32
```

When calling `p.sum()`, the compiler automatically passes `p` as the first argument.

### Type System

Methods are stored in `StructTypeInfo.methods` as a `Span<MethodInfo>`:

```cpp
struct MethodInfo {
    StringView name;
    Span<Type*> param_types;  // NOT including implicit self
    Type* return_type;
    Decl* decl;               // Points to MethodDecl AST node
};
```

The `param_types` array does NOT include `self` - it only contains the user-declared parameters.

### AST Representation

Methods use a dedicated AST node:

```cpp
// In AstKind enum
DeclMethod

// MethodDecl struct
struct MethodDecl {
    StringView struct_name;   // "Point"
    StringView name;          // "sum"
    Span<Param> params;       // User-declared params (not self)
    TypeExpr* return_type;    // nullptr for void
    Stmt* body;
    bool is_pub;
};
```

### Compilation Pipeline

1. **Parser**: Detects `fun Identifier.` pattern and creates `DeclMethod` node
2. **Semantic Analysis** (type collection): Registers struct types
3. **Semantic Analysis** (member resolution): Creates `MethodInfo` and adds to struct
4. **Semantic Analysis** (body analysis): Type-checks method body with `self` in scope
5. **IR Builder**: Generates function IR with mangled name and `self` as first parameter
6. **Lowering**: Standard function lowering (methods are just functions)

### Method vs Constructor Disambiguation

Both method calls (`obj.method()`) and named constructor calls (`Type.ctor()`) use `GetExpr` as the callee. The compiler distinguishes them by checking if the identifier is a type name:

- If `GetExpr.object` is an identifier that resolves to a named type: **constructor call**
- Otherwise: **method call**

## Works With Heap-Allocated Structs

Methods work identically on heap-allocated structs:

```roxy
fun main(): i32 {
    var p: uniq Point = uniq Point { x = 100, y = 200 };
    print(p.sum());  // Prints 300
    delete p;
    return 0;
}
```

The `self` parameter receives a reference to the heap object, and modifications persist.

## Visibility

Methods can be marked `pub` for export:

```roxy
pub fun Point.sum(): i32 {
    return self.x + self.y;
}
```

Non-public methods are module-private.

## Limitations

- **No method overloading**: Each struct can have only one method with a given name
- **No inheritance**: Methods are not inherited (struct inheritance not yet implemented)
- **No `super`**: The `super` keyword is not yet supported for method dispatch

## Related Files

- `include/roxy/compiler/types.hpp` - `MethodInfo` struct
- `include/roxy/compiler/ast.hpp` - `DeclMethod`, `MethodDecl`
- `src/roxy/compiler/parser.cpp` - Method parsing
- `src/roxy/compiler/semantic.cpp` - Method registration and body analysis
- `src/roxy/compiler/ir_builder.cpp` - Method IR generation
- `tests/e2e/methods_test.cpp` - E2E test cases

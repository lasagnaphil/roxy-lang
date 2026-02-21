# Constructors and Destructors

This document describes the implementation of constructors and destructors in Roxy.

## Syntax

### Constructor Declaration

```roxy
// Default constructor (no name after struct)
fun new StructName(params) {
    self.field = value;
}

// Named constructor
fun new StructName.from_file(path: string) {
    // initialization logic
}

// Public constructor (can be called from other modules)
pub fun new StructName(params) {
    // ...
}
```

### Destructor Declaration

```roxy
// Default destructor
fun delete StructName() {
    // cleanup logic
}

// Named destructor (can have parameters!)
fun delete StructName.save_to(path: string) {
    // save state before cleanup
}
```

### Constructor/Destructor Calls

```roxy
// Stack allocation (returns value type)
var p: Point = Point(1, 2);
var q: Point = Point.from_coords(3, 4);

// Heap allocation (returns uniq<Type>)
var hp: uniq Point = uniq Point(1, 2);
var hq: uniq Point = uniq Point.from_coords(3, 4);

// Struct literals
var lit: Point = Point { x = 10, y = 20 };
var hlit: uniq Point = uniq Point { x = 5, y = 15 };

// Default destructor call
delete hp;

// Named destructor with arguments
delete hq.save_to("backup.dat");
```

## The `self` Keyword

Inside constructors and destructors, use `self` to access struct fields:

```roxy
struct Point {
    x: i32;
    y: i32;
}

fun new Point(x: i32, y: i32) {
    self.x = x;  // 'self' refers to the struct being constructed
    self.y = y;
}

fun delete Point() {
    print(self.x);  // Can access fields in destructor
}
```

The `self` keyword is a reference to the current struct instance (`ref<StructType>`).

## Synthesized Default Constructors

If no constructor is defined for a struct, the compiler synthesizes a default constructor that:

1. **Zero-initializes** all fields to their zero values:
   - `i32`, `i64`, `f32`, `f64` → `0`
   - `bool` → `false`
   - `string` → `""`
   - Pointers/references → `null`

2. **Uses field default values** if declared:

```roxy
struct Config {
    width: i32 = 800;   // Uses 800
    height: i32 = 600;  // Uses 600
    debug: bool;        // Zero-init to false
}

// Synthesized default constructor initializes:
// width = 800, height = 600, debug = false
var c: Config = Config();
```

## Implementation Details

### AST Nodes

```cpp
// include/roxy/compiler/ast.hpp

struct ConstructorDecl {
    StringView struct_name;  // e.g., "Point"
    StringView name;         // empty for default, e.g., "from_file" for named
    Span<Param> params;
    Stmt* body;
    bool is_pub;
};

struct DestructorDecl {
    StringView struct_name;
    StringView name;         // empty for default
    Span<Param> params;      // Destructors CAN have parameters
    Stmt* body;
    bool is_pub;
};

// Constructor calls are parsed as CallExpr with is_heap flag
struct CallExpr {
    Expr* callee;
    Span<CallArg> arguments;
    StringView constructor_name;  // For named constructors: Point.from_coords() -> "from_coords"
    bool is_heap;                 // true for "uniq Type(...)" constructor calls
};

struct DeleteStmt {
    Expr* expr;
    StringView destructor_name;   // empty for default
    Span<CallArg> arguments;      // destructors can have args
};
```

### Name Mangling

Constructors and destructors are compiled as regular functions with mangled names:

| Declaration | Mangled Name |
|-------------|--------------|
| `fun new Point()` | `Point$$new` |
| `fun new Point.from_coords(...)` | `Point$$new$$from_coords` |
| `fun delete Point()` | `Point$$delete` |
| `fun delete Point.save_to(...)` | `Point$$delete$$save_to` |

### IR Generation

Constructors and destructors receive `self` as an implicit first parameter:

```cpp
// In ir_builder.cpp

void IRBuilder::build_constructor(Decl* decl, Type* struct_type) {
    // First parameter is 'self' pointer
    BlockParam self_param;
    self_param.value = m_current_func->new_value();
    self_param.type = m_types.ref_type(struct_type);
    self_param.name = "self";
    m_current_func->params.push_back(self_param);

    // Add 'self' to local scope
    define_local("self", self_param.value, self_param.type);

    // Generate body...
}
```

### Constructor Call Compilation

```
Point(args)        // stack allocation
uniq Point(args)   // heap allocation
    ↓
1. StackAlloc (for stack) or emit_new (for heap)
2. Call constructor: Point$$new(self_ptr, args...)
3. Return pointer/value
```

### `delete` Statement Compilation

```
delete obj.save_to(args)
    ↓
1. Call destructor: Point$$delete$$save_to(obj, args...)
2. emit_delete (free memory)
```

## Type System Integration

Constructor and destructor information is stored in `StructTypeInfo`:

```cpp
struct ConstructorInfo {
    StringView name;           // empty for default
    Span<Type*> param_types;
    Decl* decl;
    bool is_pub;
};

struct DestructorInfo {
    StringView name;
    Span<Type*> param_types;
    Decl* decl;
    bool is_pub;
};

struct StructTypeInfo {
    // ... existing fields ...
    Vector<ConstructorInfo> constructors;
    Vector<DestructorInfo> destructors;
};
```

## Semantic Analysis

The semantic analyzer performs these checks:

1. **Struct existence**: The struct name in constructor/destructor must be defined
2. **Duplicate names**: No two constructors (or destructors) can have the same name
3. **Parameter type resolution**: All parameter types must be valid
4. **`self` context**: `self` can only be used inside constructors/destructors
5. **Constructor lookup**: `new` expressions must reference valid constructors
6. **Destructor lookup**: `delete` statements must reference valid destructors
7. **Argument matching**: Arguments must match constructor/destructor parameters

## Examples

### Basic Constructor/Destructor

```roxy
struct Counter {
    value: i32;
}

fun new Counter(initial: i32) {
    self.value = initial;
}

fun delete Counter() {
    print(self.value);  // Print final value on cleanup
}

fun main(): i32 {
    var c: uniq Counter = uniq Counter(42);
    delete c;  // Prints "42"
    return 0;
}
```

### Multiple Constructors

```roxy
struct Point {
    x: i32;
    y: i32;
}

fun new Point() {
    self.x = 0;
    self.y = 0;
}

fun new Point.from_coords(x: i32, y: i32) {
    self.x = x;
    self.y = y;
}

fun new Point.from_value(val: i32) {
    self.x = val;
    self.y = val;
}

fun main(): i32 {
    var p1: Point = Point();                  // (0, 0)
    var p2: Point = Point.from_coords(3, 4);  // (3, 4)
    var p3: Point = Point.from_value(5);      // (5, 5)
    return 0;
}
```

### Named Destructor with Parameters

```roxy
struct Resource {
    id: i32;
}

fun new Resource(id: i32) {
    self.id = id;
}

fun delete Resource() {
    print(self.id);
}

fun delete Resource.with_message(msg: i32) {
    print(msg);
    print(self.id);
}

fun main(): i32 {
    var r1: uniq Resource = uniq Resource(1);
    var r2: uniq Resource = uniq Resource(2);

    delete r2.with_message(999);  // Prints "999" then "2"
    delete r1;                     // Prints "1"
    return 0;
}
```

## Related Files

| File | Description |
|------|-------------|
| `include/roxy/compiler/ast.hpp` | AST node definitions |
| `src/roxy/compiler/parser.cpp` | Parser for constructor/destructor syntax |
| `src/roxy/compiler/semantic.cpp` | Semantic analysis and type checking |
| `src/roxy/compiler/ir_builder.cpp` | IR generation for constructors/destructors |
| `tests/e2e/test_constructors.cpp` | End-to-end tests |

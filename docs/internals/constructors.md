# Constructors and Destructors

Roxy structs support named constructors and destructors, declared with `fun new` / `fun delete`. They compile to ordinary functions with mangled names that take `self` as an implicit first parameter — there is no constructor-specific runtime. A struct may declare multiple constructors and destructors (distinguished by name); destructors may take parameters.

## Syntax

### Declarations

```roxy
fun new Point(x: i32, y: i32) {     // default constructor (no name)
    self.x = x;
    self.y = y;
}

fun new Point.from_file(path: string) { ... }   // named constructor
pub fun new Point(...) { ... }                  // callable from other modules

fun delete Point() { ... }                      // default destructor
fun delete Point.save_to(path: string) { ... }  // named destructor (may take params)
```

Inside a constructor or destructor, `self` is a `ref<StructType>` to the instance being built or torn down, used to read and write fields.

### Calls

```roxy
var p: Point        = Point(1, 2);            // stack allocation (value type)
var q: Point        = Point.from_coords(3, 4);
var hp: uniq Point  = uniq Point(1, 2);       // heap allocation (uniq<Point>)
var hq: uniq Point  = uniq Point.from_coords(3, 4);

var lit: Point      = Point { x = 10, y = 20 };       // struct literal
var hlit: uniq Point = uniq Point { x = 5, y = 15 };

delete hp;                       // default destructor
delete hq.save_to("backup.dat"); // named destructor with arguments
```

The same constructor works for both stack and heap allocation; `uniq` selects heap.

## Synthesized Default Constructors

If a struct declares no constructor, the compiler synthesizes one that initializes each field to its declared default value, or to its zero value when no default is given (`0` for numerics, `false` for `bool`, `""` for `string`, `null` for pointers/references).

```roxy
struct Config {
    width: i32 = 800;   // 800
    height: i32 = 600;  // 600
    debug: bool;        // zero-init to false
}

var c: Config = Config();   // width=800, height=600, debug=false
```

## Name Mangling

Constructors and destructors are compiled as regular functions with mangled names:

| Declaration | Mangled Name |
|-------------|--------------|
| `fun new Point()` | `Point$$new` |
| `fun new Point.from_coords(...)` | `Point$$new$$from_coords` |
| `fun delete Point()` | `Point$$delete` |
| `fun delete Point.save_to(...)` | `Point$$delete$$save_to` |

## How It Works

Constructor calls are parsed as `CallExpr`s carrying the (possibly empty) constructor name and an `is_heap` flag (`uniq Type(...)`); `delete` is a `DeleteStmt` carrying the destructor name and arguments. Per-struct constructor and destructor metadata (name, parameter types, declaring `Decl`, `is_pub`) lives in `StructTypeInfo`. See `compiler/ast.hpp` and `compiler/types.hpp`.

Each constructor and destructor receives `self` as an implicit first parameter — the IR builder prepends a `ref<struct_type>` block parameter named `self` and binds it in the local scope before generating the body (`ir_builder.cpp`).

Call compilation:

- **Constructor call** — `StackAlloc` (stack) or `emit_new` (heap) for the instance, then `Point$$new(self_ptr, args...)`, then return the pointer/value.
- **`delete` statement** — call the named destructor `Point$$delete$$save_to(obj, args...)`, then `emit_delete` to free memory.

### Implicit destruction at scope exit

When a `uniq` variable leaves scope without being explicitly deleted or moved, the compiler emits cleanup automatically: call the default destructor `Point$$delete(obj)` (if one exists), `emit_delete`, and mark `obj` as moved to prevent a double-delete. With no default destructor, only `emit_delete` runs. Cleanup is LIFO (last declared, first destroyed). See [lifetimes.md §17](lifetimes.md) for RAII semantics.

## Semantic Analysis

The analyzer checks that: the named struct exists; no two constructors (or destructors) share a name; parameter types resolve; `self` appears only inside constructors/destructors; `new` expressions and `delete` statements reference valid constructors/destructors; and arguments match the resolved parameters.

## Examples

### Basic constructor/destructor

```roxy
struct Counter {
    value: i32;
}

fun new Counter(initial: i32) {
    self.value = initial;
}

fun delete Counter() {
    print(self.value);   // print final value on cleanup
}

fun main(): i32 {
    var c: uniq Counter = uniq Counter(42);
    delete c;            // prints "42"
    return 0;
}
```

### Multiple constructors

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

fun main(): i32 {
    var p1: Point = Point();                 // (0, 0)
    var p2: Point = Point.from_coords(3, 4); // (3, 4)
    return 0;
}
```

### Named destructor with parameters

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

    delete r2.with_message(999);  // prints "999" then "2"
    delete r1;                     // prints "1"
    return 0;
}
```

## Files

| File | Purpose |
|------|---------|
| `include/roxy/compiler/ast.hpp` | `ConstructorDecl` / `DestructorDecl` / `CallExpr` / `DeleteStmt` |
| `include/roxy/compiler/types.hpp` | `ConstructorInfo` / `DestructorInfo` in `StructTypeInfo` |
| `src/roxy/compiler/parser.cpp` | constructor/destructor syntax and call parsing |
| `src/roxy/compiler/semantic.cpp` | semantic analysis and type checking |
| `src/roxy/compiler/ir_builder.cpp` | IR generation, implicit `self`, scope-exit cleanup |
| `tests/e2e/test_constructors.cpp` | E2E tests |

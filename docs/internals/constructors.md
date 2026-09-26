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

Constructors and destructors compile to regular functions taking `self` as an implicit first parameter (see [methods.md](methods.md)); a constructor call allocates the instance (stack, or heap for `uniq`) and then calls the mangled function, and `delete` calls the destructor and then frees:

| Declaration | Mangled Name |
|-------------|--------------|
| `fun new Point()` | `Point$$new` |
| `fun new Point.from_coords(...)` | `Point$$new$$from_coords` |
| `fun delete Point()` | `Point$$delete` |
| `fun delete Point.save_to(...)` | `Point$$delete$$save_to` |

## Implicit destruction at scope exit

When a `uniq` variable leaves scope without being explicitly deleted or moved, the compiler runs the default destructor `Point$$delete` (if one exists) and frees the object; cleanup is LIFO (last declared, first destroyed). See [lifetimes.md → RAII, moves, and `borrowed`](lifetimes.md#raii-moves-and-borrowed) for RAII semantics.

## Example: named destructor with parameters

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

**Tests:** `tests/e2e/test_constructors.cpp`

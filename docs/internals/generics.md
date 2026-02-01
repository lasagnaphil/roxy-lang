# Generics

> **Note:** This feature has not been implemented yet. This document describes the planned design.

> **Implementation Priority:** Generics are a **lower priority** than traits. Roxy prioritizes fast compile times, and full monomorphization can significantly impact compilation speed. Consider implementing traits first (which solve most polymorphism needs like `print()`) and adding generics later if needed. See the "Compile Time Considerations" section below.

> **Design Decision:** Common container types (`Array`, `Dict`, `Option`, etc.) are **built-in compiler types** that don't require monomorphization. User-defined generics are available for custom containers but are rarely needed. See "Built-in vs User-Defined Generics" section below.

Generics enable writing code that works with multiple types while maintaining static type safety.

## Syntax Overview

```roxy
// Generic function
fun identity<T>(value: T): T {
    return value;
}

// Generic struct
struct Box<T> {
    value: T;
}

// Generic method
fun Box.unwrap<T>(): T {
    return self.value;
}

// Usage
var x: i32 = identity(42);           // T inferred as i32
var b: Box<string> = Box { value = "hello" };
```

## Generic Functions

### Basic Syntax

```roxy
fun swap<T>(a: inout T, b: inout T) {
    var temp: T = a;
    a = b;
    b = temp;
}

fun first<T>(arr: T[]): T {
    return arr[0];
}

fun make_pair<T, U>(a: T, b: U): Pair<T, U> {
    return Pair { first = a, second = b };
}
```

### Type Parameter Constraints (Trait Bounds)

```roxy
// Single bound
fun print_value<T: Printable>(value: T) {
    value.print();
}

// Multiple bounds
fun compare_and_print<T: Printable + Comparable>(a: T, b: T) {
    if (a.lt(b)) {
        a.print();
    } else {
        b.print();
    }
}

// Multiple type parameters with different bounds
fun process<T: Printable, U: Comparable>(x: T, y: U) {
    x.print();
    // ...
}
```

### Type Inference

The compiler infers type arguments when possible:

```roxy
var x: i32 = identity(42);        // T inferred as i32
var y: string = identity("hi");   // T inferred as string

// Explicit type arguments (when inference fails or for clarity)
var z: i32 = identity<i32>(42);
```

## Generic Structs

### Basic Syntax

```roxy
struct Box<T> {
    value: T;
}

struct Pair<T, U> {
    first: T;
    second: U;
}

struct Triple<T> {
    a: T;
    b: T;
    c: T;
}
```

### Generic Struct Methods

Methods on generic structs use the same type parameters:

```roxy
struct Box<T> {
    value: T;
}

fun Box.get<T>(): T {
    return self.value;
}

fun Box.set<T>(new_value: T) {
    self.value = new_value;
}

fun Box.map<T, U>(f: fun(T): U): Box<U> {
    return Box { value = f(self.value) };
}
```

### Generic Struct Constructors

```roxy
struct Box<T> {
    value: T;
}

fun new Box<T>(value: T) {
    self.value = value;
}

fun new Box.empty<T>() {
    // Requires T to have a default value or be nullable
}

// Usage
var b1: Box<i32> = Box(42);
var b2: Box<string> = Box("hello");
```

### Trait Bounds on Generic Structs

```roxy
// Require T to implement Printable
struct PrintableBox<T: Printable> {
    value: T;
}

fun PrintableBox.print_contents<T: Printable>() {
    self.value.print();
}
```

## Generic Type Aliases (Optional)

```roxy
type IntBox = Box<i32>;
type StringPair = Pair<string, string>;
type Result<T> = Pair<T, bool>;  // Generic alias
```

## The `Self` Type

Inside methods and trait implementations, `Self` refers to the implementing type:

```roxy
trait Clonable;
fun Clonable.clone(): Self;

struct Point { x: i32; y: i32; }

fun Point.clone(): Point for Clonable {  // Self = Point
    return Point { x = self.x, y = self.y };
}
```

For generic types, `Self` includes the type parameters:

```roxy
struct Box<T> { value: T; }

fun Box.clone<T: Clonable>(): Box<T> for Clonable {  // Self = Box<T>
    return Box { value = self.value.clone() };
}
```

## Common Generic Patterns

### Optional/Nullable Values

```roxy
struct Option<T> {
    has_value: bool;
    value: T;
}

fun Option.some<T>(value: T): Option<T> {
    return Option { has_value = true, value = value };
}

fun Option.none<T>(): Option<T> {
    return Option { has_value = false };
}

fun Option.unwrap<T>(): T {
    if (!self.has_value) {
        // panic or error
    }
    return self.value;
}

fun Option.unwrap_or<T>(default: T): T {
    if (self.has_value) {
        return self.value;
    }
    return default;
}
```

### Result Type

```roxy
struct Result<T, E> {
    is_ok: bool;
    value: T;
    error: E;
}

fun Result.ok<T, E>(value: T): Result<T, E> {
    return Result { is_ok = true, value = value };
}

fun Result.err<T, E>(error: E): Result<T, E> {
    return Result { is_ok = false, error = error };
}
```

### Generic Collections

```roxy
struct List<T> {
    data: T[];
    length: i32;
}

fun List.push<T>(item: T) {
    // ...
}

fun List.pop<T>(): T {
    // ...
}

fun List.get<T>(index: i32): T {
    return self.data[index];
}

fun List.map<T, U>(f: fun(T): U): List<U> {
    var result: List<U> = List { };
    for (var i: i32 = 0; i < self.length; i = i + 1) {
        result.push(f(self.get(i)));
    }
    return result;
}
```

## Grammar

```
generic_params  -> "<" type_param_list ">" ;
type_param_list -> type_param ( "," type_param )* ;
type_param      -> Identifier ( ":" trait_bounds )? ;
trait_bounds    -> Identifier ( "+" Identifier )* ;

generic_args    -> "<" type_list ">" ;
type_list       -> type_expr ( "," type_expr )* ;

// Updated declarations
fun_decl        -> ( "pub" )? "fun" Identifier generic_params?
                   "(" parameters? ")" ( ":" type_expr )?
                   ( block | ";" ) ;

struct_decl     -> "struct" Identifier generic_params?
                   ( ":" Identifier generic_args? )?
                   "{" field_decl* "}" ;

method_decl     -> ( "pub" )? "fun" Identifier "." Identifier generic_params?
                   "(" parameters? ")" ( ":" type_expr )?
                   block ;

// Type expressions can include generic arguments
type_expr       -> ( "uniq" | "ref" | "weak" )? Identifier generic_args? ( "[" "]" )? ;
```

## Compile Time Considerations

**Fast compile times are a core goal for Roxy.** This section discusses the trade-offs of different generic implementation strategies.

### The Problem with Monomorphization

Monomorphization generates specialized code for each type instantiation:

```roxy
fun identity<T>(x: T): T { return x; }

identity(42);       // Generates identity$i32
identity("hello");  // Generates identity$string
identity(3.14);     // Generates identity$f64
```

For a `List<T>` used with 20 different types, that's 20× the compilation work for all List methods. This is why Rust and C++ have notoriously slow compile times.

### Alternative Strategies

| Strategy | Compile Time | Runtime | Value Semantics |
|----------|--------------|---------|-----------------|
| Monomorphization | Slow | Fast | Preserved |
| Type erasure (Java/Go) | Fast | Slower (boxing) | Lost |
| Interpreter-level | Fastest | Slowest | Preserved |

### Recommendation: Traits First, Built-in Containers

For Roxy's use case (scripting language, game engines, fast iteration):

1. **Implement traits first**
   - Trait method implementations monomorphize once at definition time
   - Solves `print()`, `==`, `<` and most polymorphism needs
   - No instantiation explosion at call sites

2. **Provide built-in parameterized containers**
   - `T[]`, `Dict<K,V>`, `Option<T>`, `Pair<T,U>`, etc.
   - Uniform runtime representation (no monomorphization)
   - Covers 90%+ of generic use cases

3. **Add user-defined generics last (if ever)**
   - Only needed for custom container types
   - Uses monomorphization but rarely triggered
   - Many projects won't need this at all

### If Generics Are Implemented

Use aggressive caching to minimize redundant work:
- Cache instantiations across modules
- Share identical instantiations (e.g., `List<i32>` compiled once, reused everywhere)
- Consider lazy instantiation (only compile what's actually called)

## Built-in vs User-Defined Generics

Roxy uses a **hybrid approach** to minimize monomorphization cost while still supporting user-defined generics.

### Built-in Parameterized Types

Common container types are **compiler built-ins** with uniform runtime representations. They don't go through monomorphization.

| Type | Syntax | Purpose |
|------|--------|---------|
| Array | `T[]` | Dynamic array |
| Dict | `Dict<K, V>` | Hash map |
| Set | `Set<T>` | Unique elements |
| Pair | `Pair<T, U>` | Two-element tuple |
| Triple | `Triple<T, U, V>` | Three-element tuple |
| Option | `Option<T>` | Nullable value wrapper |
| Result | `Result<T, E>` | Success/error value |
| Stack | `Stack<T>` | LIFO collection |
| Queue | `Queue<T>` | FIFO collection |

**Implementation:**
- Compiler has special knowledge of these types
- Runtime uses type-erased representations with stored element sizes
- Type safety enforced at compile time, uniform operations at runtime
- No code duplication per element type

```roxy
// All built-in - no monomorphization cost
var items: i32[] = array_new(10);
var scores: Dict<string, i32> = dict_new();
var point: Pair<i32, i32> = Pair(10, 20);
var maybe: Option<string> = Option.some("hello");
var result: Result<i32, string> = Result.ok(42);
```

### User-Defined Generics

Users can still define their own generic types when needed. These use monomorphization:

```roxy
// User-defined - monomorphized per instantiation
struct Graph<T> {
    nodes: T[];
    edges: Pair<i32, i32>[];  // Uses built-in Pair
}

fun Graph.add_node<T>(value: T) {
    // ...
}

var g: Graph<string> = ...;  // Generates Graph$string
```

### Why This Hybrid Approach?

| Aspect | Built-in Types | User Generics |
|--------|----------------|---------------|
| Compile cost | None (uniform runtime) | Per-instantiation |
| Code size | Fixed | Grows with usage |
| Usage frequency | Very common | Rare |
| Flexibility | Fixed set | Unlimited |

Since built-in types cover 90%+ of generic usage, most Roxy code compiles fast. User-defined generics are available for custom data structures but incur compile-time cost only when used.

### Runtime Representation

Built-in containers use a uniform layout that works for any element type:

```cpp
// Array header - same struct for i32[], string[], Point[], etc.
struct ArrayHeader {
    u32 length;
    u32 capacity;
    u32 element_size;   // Bytes per element
    u32 element_slots;  // Slots per element (for GC/copying)
};
// Element data follows header
```

Operations use `element_size` to compute offsets at runtime:
```cpp
void* get_element(ArrayHeader* arr, u32 index) {
    return (char*)(arr + 1) + index * arr->element_size;
}
```

This is slightly slower than monomorphized code but avoids compile-time explosion.

## Implementation Strategy

### Monomorphization

If full generics are implemented, Roxy would use monomorphization (like Rust, C++, Zig) rather than type erasure (like Java, Go):

```roxy
fun identity<T>(x: T): T { return x; }

identity(42);       // Generates: identity$i32
identity("hello");  // Generates: identity$string
identity(true);     // Generates: identity$bool
```

**Advantages:**
- Zero runtime overhead
- Full optimization per instantiation
- No boxing required

**Disadvantages:**
- Code size increases with instantiations
- Longer compile times

### Name Mangling

Generic instantiations use type-encoded names:

```
function<T>       -> function$T
function<T, U>    -> function$T$U
Struct<T>         -> Struct$T
Struct<T>.method  -> Struct$T$$method
```

Examples:
- `identity<i32>` → `identity$i32`
- `Box<string>` → `Box$string`
- `Pair<i32, f64>` → `Pair$i32$f64`
- `Box<i32>.get` → `Box$i32$$get`

Nested generics:
- `Box<Box<i32>>` → `Box$Box$i32`
- `List<Pair<i32, string>>` → `List$Pair$i32$string`

### Instantiation Process

1. **Collection Phase**: Gather all generic function/struct usages
2. **Inference Phase**: Resolve type arguments for each usage
3. **Instantiation Phase**: Generate specialized code for each unique type combination
4. **Validation Phase**: Type-check each instantiation

### Struct Layout

Generic structs compute layout per instantiation:

```roxy
struct Pair<T, U> {
    first: T;
    second: U;
}

// Pair<i32, i32>: 2 slots (8 bytes)
// Pair<i64, i32>: 3 slots (12 bytes)
// Pair<Point, f64>: 4 slots (16 bytes) if Point is 2 slots
```

## Interaction with Other Features

### Generics + Traits

```roxy
trait Printable;
fun Printable.print();

fun print_all<T: Printable>(items: T[]) {
    for (var i: i32 = 0; i < array_len(items); i = i + 1) {
        items[i].print();
    }
}
```

### Generics + Inheritance

```roxy
struct Animal { hp: i32; }
struct Dog : Animal { breed: i32; }

struct Cage<T: Animal> {
    occupant: T;
}

var dog_cage: Cage<Dog> = Cage { occupant = Dog { hp = 100, breed = 5 } };
```

### Generics + Constructors

```roxy
struct Container<T> {
    items: T[];
}

fun new Container<T>() {
    self.items = array_new<T>(0);
}

fun new Container.with_capacity<T>(cap: i32) {
    self.items = array_new<T>(cap);
}
```

## Limitations (Initial Implementation)

To reduce complexity, the initial implementation may have these limitations:

1. **No higher-kinded types**: Can't abstract over type constructors
   ```roxy
   // NOT supported:
   fun map<F<_>, T, U>(container: F<T>, f: fun(T): U): F<U>
   ```

2. **No associated types**: Traits can't define type members
   ```roxy
   // NOT supported:
   trait Iterator {
       type Item;
       fun next(): Option<Item>;
   }
   ```

3. **No specialization**: Can't provide different implementations for specific types
   ```roxy
   // NOT supported:
   fun process<T>(x: T) { /* generic impl */ }
   fun process<i32>(x: i32) { /* specialized impl */ }
   ```

4. **No const generics**: Can't parameterize by values
   ```roxy
   // NOT supported:
   struct Array<T, N: i32> { data: T[N]; }
   ```

These can be added in future versions if needed.

## Error Messages

### Type Mismatch

```
error: type mismatch in generic function
  --> main.roxy:10:5
   |
   | var x: string = identity(42);
   |                 ^^^^^^^^^^^^ expected 'string', found 'i32'
   |
   = note: in instantiation of 'identity<i32>'
```

### Missing Trait Bound

```
error: trait bound not satisfied
  --> main.roxy:15:5
   |
   | print_value(my_struct);
   | ^^^^^^^^^^^^^^^^^^^^^^ 'MyStruct' does not implement 'Printable'
   |
   = note: required by bound 'T: Printable' in 'print_value<T>'
   = help: implement 'Printable' for 'MyStruct':
           fun MyStruct.print() for Printable { ... }
```

### Cannot Infer Type

```
error: cannot infer type parameter
  --> main.roxy:20:5
   |
   | var x = Option.none();
   |         ^^^^^^^^^^^^^ cannot infer type 'T'
   |
   = help: specify the type explicitly: Option.none<i32>()
```

## Dependencies

Implementing generics requires:

1. **Type parameter tracking** in AST and type system
2. **Type inference** for argument deduction
3. **Instantiation cache** to avoid duplicate code generation
4. **Name mangling** for unique function/struct names
5. **Trait integration** for bounded polymorphism

## Files (Planned)

| File | Purpose |
|------|---------|
| `include/roxy/compiler/generics.hpp` | Generic type representation |
| `src/roxy/compiler/generics.cpp` | Instantiation and inference |
| `src/roxy/compiler/semantic.cpp` | Generic type checking |
| `src/roxy/compiler/ir_builder.cpp` | Monomorphization |
| `tests/e2e/generics_test.cpp` | E2E tests |

## Implementation Order

**Recommended overall order:**

1. **Traits first (see `traits.md`)** - Solves most polymorphism needs without generics
2. **Generics later (if needed)** - Only add if users require generic functions/structs

**If generics are implemented:**

1. **Phase 1**: Generic functions (no bounds)
   - Parse `<T>` syntax
   - Type parameter substitution
   - Basic monomorphization

2. **Phase 2**: Generic structs
   - Struct type parameters
   - Per-instantiation layout computation
   - Generic constructors

3. **Phase 3**: Trait bounds
   - Integrate with trait system
   - Bound checking at instantiation

4. **Phase 4**: Type inference
   - Infer type arguments from usage
   - Error recovery for ambiguous cases

**Consider skipping generics entirely** if:
- Compile time is paramount
- Named functions (`list_i32_push`) are acceptable
- Traits alone cover the use cases
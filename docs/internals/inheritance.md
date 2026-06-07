# Struct Inheritance

Roxy supports single inheritance for structs with static dispatch (no vtables). Children inherit all parent fields and methods, can override methods, and reach parents via `super`. Method calls resolve at compile time from the variable's declared type; reference types are covariant.

## Fields

A child declared with `: Parent` inherits every parent field. Fields are laid out parent-first, so a child value is a valid prefix-compatible layout of its parent:

```roxy
struct Animal  { hp: i32; }
struct Dog : Animal     { breed: i32; }
struct Labrador : Dog   { color: i32; }
```

```
Labrador memory layout:
+--------+--------+--------+
|   hp   |  breed |  color |
| (i32)  |  (i32) |  (i32) |
+--------+--------+--------+
  slot 0   slot 1   slot 2
   Animal    Dog     own
```

All inherited fields are accessible on the child (`lab.hp`, `lab.breed`, `lab.color`).

## Methods

Methods are inherited and may be overridden by redeclaring them on the child:

```roxy
fun Animal.speak(): i32 { return 1; }
fun Dog.speak(): i32    { return 2; }   // overrides Animal.speak

var d: Dog = Dog { hp = 100, breed = 5 };
print(d.speak());  // 2
```

### Static dispatch and value slicing

Calls resolve from the **declared type** of the variable, not the runtime value — there is no virtual dispatch. Assigning a child to a parent variable copies only the parent fields (value slicing), and subsequent calls use the parent's method:

```roxy
var d: Dog = Dog { hp = 100, breed = 5 };
d.speak();          // Dog.speak  -> 2

var a: Animal = d;  // value slicing: copies only hp
a.speak();          // Animal.speak -> 1
```

## `super`

### Method calls

`super.method()` calls the immediate parent's version. For multi-level chains, `super` always refers to the direct parent:

```roxy
fun Animal.get_type(): i32 { return 1; }
fun Dog.get_type(): i32    { return super.get_type() + 10; }   // -> 11
fun Labrador.get_type(): i32 { return super.get_type() + 100; } // -> 111
```

### Constructor calls

A child constructor calls the parent constructor with `super(...)`. If no explicit `super()` appears, an implicit call to the parent's **default** constructor is inserted:

```roxy
fun new Animal(hp: i32) { self.hp = hp; }

fun new Dog(hp: i32, breed: i32) {
    super(hp);             // explicit Animal(hp)
    self.breed = breed;
}

fun new Dog(breed: i32) {
    // implicit super() — calls Animal's default constructor
    self.breed = breed;
}
```

Named parent constructors are reached with `super.name()` (e.g. `super.with_full_hp()`).

## Construction and destruction order

Constructors run **parent-first**: the (explicit or implicit) `super` call executes before the child body. Synthesized default constructors chain the same way and only initialize their own fields — the parent constructor handles inherited fields:

```roxy
fun new Animal() { self.hp = 50; print(1); }
fun new Dog(breed: i32) {            // implicit super() -> Animal()
    self.breed = breed; print(2);
}
var d: Dog = Dog(5);                 // prints 1 then 2
```

Destructors run in reverse — **child-first, then parent** — automatically; there is no explicit `super` in a destructor:

```roxy
fun delete Animal() { print(1); }
fun delete Dog()    { print(2); }
var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
delete d;                            // prints 2 then 1
```

## Subtyping

A child is a subtype of its parent. Value assignment slices (copies only parent fields). Reference types are **covariant**, so a borrow of a child satisfies a parent reference parameter:

```roxy
fun print_animal(a: ref Animal) { print(a.hp); }

var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
print_animal(d);   // uniq Dog -> ref Animal
```

Supported conversions: `uniq Child → uniq Parent`, `uniq Child → ref Parent`, `ref Child → ref Parent`.

## Implementation Details

**Type system.** `StructTypeInfo.parent` points at the parent type (or null), and `StructTypeInfo.fields` holds *all* fields (inherited + own) in parent-first order. `is_subtype_of()` walks the parent chain; `check_assignable()` permits struct subtyping (with slicing) and covariant reference conversions.

**Method lookup.** `lookup_method_in_hierarchy()` searches child-to-parent. A `super` call mangles against the parent's name.

**Chaining rules.** Constructor chaining is explicit (implicit `super()` to the parent's default when omitted); the child body runs after the parent. Destructor chaining is automatic; the child runs before the parent.

### Name mangling

| Declaration | Mangled name |
|-------------|--------------|
| `fun Animal.speak()` | `Animal$$speak` |
| `fun new Animal()` | `Animal$$new` |
| `fun new Animal.named()` | `Animal$$new$$named` |
| `fun delete Animal()` | `Animal$$delete` |

## Files

| File | Purpose |
|---|---|
| `src/roxy/compiler/types.cpp` | `StructTypeInfo.parent`, field layout, `is_subtype_of()` |
| `src/roxy/compiler/semantic.cpp` | inheritance resolution, `check_assignable()`, method lookup, `super` resolution, constructor/destructor chaining |
| `src/roxy/compiler/ir_builder.cpp` | static method dispatch, value slicing, covariant reference conversions |
| `tests/e2e/test_inheritance.cpp` | E2E tests |

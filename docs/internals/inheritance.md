# Struct Inheritance

Roxy supports single inheritance for structs with static dispatch (no vtables).

## Syntax

### Declaring Inheritance

```roxy
struct Animal {
    hp: i32;
}

struct Dog : Animal {
    breed: i32;
}

struct Labrador : Dog {
    color: i32;
}
```

### Field Inheritance

Child structs inherit all fields from parent structs. Fields are laid out in memory with parent fields first:

```
Labrador memory layout:
+--------+--------+--------+
|   hp   |  breed |  color |
| (i32)  |  (i32) |  (i32) |
+--------+--------+--------+
  slot 0   slot 1   slot 2
```

All inherited fields are accessible on the child:

```roxy
var lab: Labrador = Labrador { hp = 100, breed = 5, color = 3 };
print(lab.hp);     // 100 - inherited from Animal
print(lab.breed);  // 5 - inherited from Dog
print(lab.color);  // 3 - own field
```

## Method Inheritance

Methods are inherited from parent structs and can be overridden:

```roxy
struct Animal {
    hp: i32;
}

fun Animal.speak(): i32 {
    return 1;
}

struct Dog : Animal {
    breed: i32;
}

fun Dog.speak(): i32 {
    return 2;  // Overrides Animal.speak
}

fun main(): i32 {
    var a: Animal = Animal { hp = 50 };
    var d: Dog = Dog { hp = 100, breed = 5 };
    print(a.speak());  // 1
    print(d.speak());  // 2
    return 0;
}
```

### Static Dispatch

Method calls are resolved at compile time based on the declared type of the variable (no virtual dispatch):

```roxy
var d: Dog = Dog { hp = 100, breed = 5 };
d.speak();  // Calls Dog.speak (returns 2)

var a: Animal = d;  // Value slicing
a.speak();  // Calls Animal.speak (returns 1)
```

## Super Keyword

The `super` keyword allows calling parent methods and constructors from child structs.

### Super Method Calls

```roxy
struct Animal {
    hp: i32;
}

fun Animal.get_type(): i32 {
    return 1;
}

struct Dog : Animal {
    breed: i32;
}

fun Dog.get_type(): i32 {
    return super.get_type() + 10;  // Calls Animal.get_type()
}

fun main(): i32 {
    var d: Dog = Dog { hp = 100, breed = 5 };
    print(d.get_type());  // 11 (1 + 10)
    return 0;
}
```

For multi-level inheritance, `super` refers to the immediate parent:

```roxy
struct Labrador : Dog {
    color: i32;
}

fun Labrador.get_type(): i32 {
    return super.get_type() + 100;  // Calls Dog.get_type()
}
// Labrador.get_type() returns 111 (1 + 10 + 100)
```

### Super Constructor Calls

Child constructors can call parent constructors using `super()`:

```roxy
struct Animal {
    hp: i32;
}

fun new Animal(hp: i32) {
    self.hp = hp;
    print(1);
}

struct Dog : Animal {
    breed: i32;
}

fun new Dog(hp: i32, breed: i32) {
    super(hp);       // Explicit call to Animal(hp)
    self.breed = breed;
    print(2);
}
```

If no explicit `super()` is present, an implicit call to the parent's default constructor is made:

```roxy
fun new Dog(breed: i32) {
    // Implicit super() called here - calls Animal's default constructor
    self.breed = breed;
}
```

### Named Constructor Calls

For named parent constructors, use `super.name()`:

```roxy
fun new Animal.with_full_hp() {
    self.hp = 100;
}

fun new Dog.strong(breed: i32) {
    super.with_full_hp();  // Call named parent constructor
    self.breed = breed;
}
```

## Constructor Chaining

### Implicit Chaining

If a child constructor doesn't have an explicit `super()` call, the parent's default constructor is called automatically:

```roxy
struct Animal {
    hp: i32;
}

fun new Animal() {
    self.hp = 50;
    print(1);
}

struct Dog : Animal {
    breed: i32;
}

fun new Dog(breed: i32) {
    // Implicit super() - calls Animal()
    self.breed = breed;
    print(2);
}

fun main(): i32 {
    var d: Dog = Dog(5);
    // Output: 1, 2 (parent first, then child)
    return 0;
}
```

### Synthesized Constructors

Synthesized default constructors also chain to parent constructors:

```roxy
struct Animal {
    hp: i32 = 50;
}

struct Dog : Animal {
    breed: i32 = 1;
}

fun main(): i32 {
    var d: Dog = Dog {};
    // Animal's synthesized constructor runs first (sets hp = 50)
    // Dog's synthesized constructor runs second (sets breed = 1)
    print(d.hp);     // 50
    print(d.breed);  // 1
    return 0;
}
```

## Destructor Chaining

Destructors are automatically chained in reverse order (child first, then parent):

```roxy
struct Animal {
    hp: i32;
}

fun delete Animal() {
    print(1);
}

struct Dog : Animal {
    breed: i32;
}

fun delete Dog() {
    print(2);
}

fun main(): i32 {
    var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
    delete d;
    // Output: 2, 1 (child destructor first, then parent)
    return 0;
}
```

## Subtyping

### Value Slicing

Assigning a child value to a parent variable copies only the parent fields (value slicing):

```roxy
var d: Dog = Dog { hp = 100, breed = 5 };
var a: Animal = d;  // Copies only hp field
print(a.hp);  // 100
// a.breed is not accessible - Animal doesn't have this field
```

### Reference Subtyping (Covariance)

Reference types support covariant subtyping:

```roxy
fun print_animal(a: ref Animal) {
    print(a.hp);
}

fun main(): i32 {
    var d: uniq Dog = uniq Dog { hp = 100, breed = 5 };
    print_animal(d);  // uniq Dog -> ref Animal conversion
    delete d;
    return 0;
}
```

Supported conversions:
- `uniq Child` → `uniq Parent`
- `uniq Child` → `ref Parent`
- `ref Child` → `ref Parent`

## Implementation Details

### Type System

- `StructTypeInfo.parent` stores a pointer to the parent type (or nullptr)
- `StructTypeInfo.fields` contains ALL fields (inherited + own)
- `is_subtype_of()` helper walks the parent chain to check subtyping
- `check_assignable()` allows struct subtyping and covariant reference conversions

### Method Lookup

- `lookup_method_in_hierarchy()` searches from child to parent for methods
- Methods are mangled as `StructName$$method_name`
- Super method calls use the parent's struct name for mangling

### Constructor/Destructor Chaining

- Constructor chaining is explicit (implicit `super()` for default) - child executes after parent
- Destructor chaining is automatic - child executes before parent
- Synthesized constructors only initialize own fields (parent constructor handles inherited fields)

### Name Mangling

| Declaration | Mangled Name |
|-------------|--------------|
| `fun Animal.speak()` | `Animal$$speak` |
| `fun new Animal()` | `Animal$$new` |
| `fun new Animal.named()` | `Animal$$new$$named` |
| `fun delete Animal()` | `Animal$$delete` |

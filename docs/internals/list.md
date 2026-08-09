# Lists

Roxy supports dynamically-sized lists with bounds checking, using generic syntax `List<T>`.

## List Layout

Lists are stored as objects with a `ListHeader` — the unified `roxy_list_header` from `roxy_rt.h`, shared by both VM and AOT-compiled programs. The header carries `length`, `capacity`, `element_slot_count` (u32 slots per element), an `element_is_inline` flag (primitives are packed in slots; structs are passed by pointer), and a pointer to a separate elements buffer. See `rt/roxy_rt.h` for the full definition.

```
// Memory layout: [ObjectHeader][ListHeader]
// Elements buffer: [u32 * capacity * element_slot_count] (separate allocation)
```

The key design choice: elements are stored in a **separate buffer** rather than inline after the header. This allows `push` to realloc the elements buffer without moving the ObjectHeader (which would invalidate all pointers to the list).

## Construction

```roxy
// Empty list (capacity 0)
var lst: List<i32> = List<i32>();

// List with pre-allocated capacity
var lst: List<i32> = List<i32>(10);
```

`List<T>` is registered as a **generic native type** via `NativeRegistry::register_generic_type`. When the compiler encounters `List<i32>`, the semantic analyzer recognizes it as a native generic, creates a monomorphized struct type (`List$i32`), and resolves its methods and constructor from the registry using `instantiate_generic_methods` / `instantiate_generic_constructor`. Since all Roxy Values are 64-bit, a single set of runtime native functions handles all element types uniformly.

## Methods

| Method | Signature | Description |
|--------|-----------|-------------|
| `len()` | `() -> i32` | Return number of elements |
| `cap()` | `() -> i32` | Return allocated capacity |
| `push(val)` | `(T) -> void` | Append element (grows if needed) |
| `pop()` | `() -> T` | Remove and return last element |
| `to_string()` | `() -> string` | `"[1, 2, 3]"` — requires `T` Printable (see below) |

## Printable: the synthesized `to_string`

`List<T>` implements `Printable` **structurally**: it is printable iff `T` is
(recursively — `List<List<i32>>` works). `f"{items}"`, `items.to_string()`,
and `print(items)` all format as `[e1, e2, ...]` with elements rendered
exactly as f-string interpolation renders them (strings unquoted; enums by
discriminant; structs via their `for Printable` impl; `[]` when empty).

The conversion is a **compiler-synthesized per-instantiation IR function**
(`List$i32$$to_string`, module-local), not a runtime native: the IR builder's
`request_container_to_string` memoizes a name per interned container type and
a `build_container_to_strings` drain phase emits the bodies after all other
function builds (nested requests grow the worklist). The body pins the
container (`ContainerPin` — a user `to_string` reached through an element that
mutates the list traps instead of dangling), loops `0..len` via `List$$len` +
`IndexGet`, converts each element through the shared `emit_to_string_value`
dispatch, and folds with `str_concat`, releasing each previous accumulator and
owned piece (string literals are immortal, so `[]` falls out of releasing the
initial `"["`). Because the body is plain IR, **both backends get it for
free**. Non-printable element types (`List<ref T>`, `uniq`, functions, narrow
ints) are rejected at the call site.

## Indexing

List indexing (`list[i]` and `list[i] = val`) is handled via native `index` and `index_mut` methods registered through `NativeRegistry::bind_generic_method`. The compiler resolves these through `TypeCache::lookup_method()` and emits `CallNative` IR ops, the same path used by all other list methods (`.len()`, `.push()`, etc.). Both perform null checks and bounds checking, setting `vm->error` on failure.

`index` is typed `fun List<T>.index(idx: i32): borrowed T` — the `borrowed` modifier demotes the element type to a borrow so `index` yields a *view*, not a transfer. For `List<uniq Point>` the result is `ref Point`, so `var x: uniq Point = list[i]` is a `ref → uniq` type error (you can't move an element out from under the list; borrow it or `pop()` it). For copyable `T` (`List<i32>`) `borrowed T` is just `T`, so indexing copies as before. The modifier is native-signature-only — it is not spellable in user source. See [lifetimes.md → The `borrowed` type modifier](lifetimes.md#the-borrowed-type-modifier).

An out-of-bounds `list[i]` **read** (`i < 0` or `i >= len`) throws a catchable
`IndexError` rather than aborting — the IR builder emits a cheap in-IR bounds
check (`List$$len` is a header read) that branches to a `throw IndexError` block
before the element read. See [exceptions.md → Index-operator
exceptions](exceptions.md#index-operator-exceptions). `.pop()` and the
`inout`/`out` element-borrow lvalue path still trap on an empty/out-of-range
access.

## Copy and Move Semantics

A `List<T>` is **always noncopyable**, whatever `T` is: it owns a heap element buffer, so — like `uniq` — it is move-only. Lists use move semantics, the same rules as `uniq` variables and value structs with destructors:

- **Passing to a function by value** moves ownership; the caller's variable is consumed
- **Initializing a new variable** (`var copy = items`) moves the source
- **Use-after-move** is a compile-time error
- **Struct fields** of list type trigger a synthetic destructor on the containing struct

`.copy()` produces an independent duplicate when one is genuinely wanted (element-wise; rejected when `T` isn't copyable).

### Borrowing a list

A parameter typed `ref List<T>` **borrows** rather than moves — the caller keeps its list and can pass it again, or pass it twice in one call:

```roxy
fun total(xs: ref List<i32>): i32 { ... }   // borrows
fun consume(xs: List<i32>): i32 { ... }     // moves

var xs: List<i32> = List<i32>();
print(f"{total(xs)} {total(xs)}");          // fine — no call-site marker needed
```

A list value *is* the pointer to its slab-allocated header, so this is `uniq → ref` with a different pointee: the conversion is implicit, the count lives in the same `ObjectHeader.ref_count`, and the free-trap is the same one. `ref` is a borrow, not an *immutable* borrow — `xs.push(1)` through it is legitimate; what it cannot do is reassign the caller's slot (that is `inout`) or be moved out of the borrowing frame. See [lifetimes.md → Containers are borrowable](lifetimes.md#containers-are-borrowable).

### Scope-Exit Cleanup (RAII)

When a noncopyable list goes out of scope, the compiler emits a cleanup loop in the IR:

1. Get the list length
2. For each element: load via `List$$index`, destroy via the element type's destructor + `Delete` (for `uniq` elements)
3. Call `List$$delete` to free the element buffer
4. Call `Delete` to free the slab-allocated list header

This follows the same block-argument loop pattern as `gen_for_stmt` in the IR builder.

## Growth Strategy

When pushing beyond capacity, the list doubles its capacity (minimum 8 elements).

## Usage Example

```roxy
fun main(): i32 {
    var lst: List<i32> = List<i32>();
    lst.push(5);
    lst.push(2);
    lst.push(8);

    for (var i: i32 = 0; i < lst.len(); i = i + 1) {
        print(lst[i]);
    }
    return 0;
}
```

## Files

- `include/roxy/rt/roxy_rt.h` - Unified `roxy_list_header` + `roxy_list_*` C API
- `src/roxy/rt/roxy_rt.cpp` - List allocation/push/pop/get/set implementation shared between VM and AOT
- `include/roxy/vm/list.hpp` - `ListHeader` typedef alias of `roxy_list_header`, VM-side declarations
- `src/roxy/vm/list.cpp` - Thin shim around `roxy_list_*` (preserves the VM's nullptr+error contract on bounds violations)
- `include/roxy/vm/natives.hpp` - Built-in native function constants
- `src/roxy/vm/natives.cpp` - Native function implementations (List<T> generic type registration)
- `include/roxy/vm/binding/registry.hpp` - Generic type registration API

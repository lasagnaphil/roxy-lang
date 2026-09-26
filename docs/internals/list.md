# Lists

Roxy supports dynamically-sized lists with bounds checking, using generic syntax `List<T>`.

## List Layout

A list is a slab object `[ObjectHeader][roxy_list_header]` (`rt/roxy_rt.h`, shared by the VM and AOT programs; `ListHeader` in `vm/list.hpp` is an alias). The header points to a separately allocated element buffer of `capacity * element_slot_count` u32 slots. The key design choice: elements are stored in a **separate buffer** rather than inline after the header. This lets `push` realloc the buffer without moving the ObjectHeader (which would invalidate all pointers to the list). Growth doubles capacity (minimum 8).

## Registration and Methods

`List<T>` is a **generic native type** ([interop.md](interop.md#generic-native-types)): `List<i32>` monomorphizes to a struct type `List$i32` whose constructor (`List<T>()` / `List<T>(cap)`) and methods are instantiated from the registry. The method set and signatures are the `bind_method` calls in `register_builtin_natives` (`src/roxy/vm/natives.cpp`); `to_string` is not among them — it is synthesized (below).

## Printable: the synthesized `to_string`

`List<T>` implements `Printable` **structurally**: it is printable iff `T` is
(recursively — `List<List<i32>>` works). `f"{items}"`, `items.to_string()`,
and `print(items)` all format as `[e1, e2, ...]` with elements rendered
exactly as f-string interpolation renders them (strings unquoted; enums by
discriminant; structs via their `for Printable` impl; `[]` when empty).

The conversion is a **compiler-synthesized per-instantiation IR function**
(`List$i32$$to_string`, module-local), not a runtime native: the IR builder's
`request_container_to_string` memoizes one per interned container type and
emits the bodies after all other functions. The body **pins the container**
(`ContainerPin`), so a user `to_string` reached through an element that mutates
the list traps instead of dangling. Because the body is plain IR, **both
backends get it for free**. Non-printable element types (`List<ref T>`, `uniq`, functions, narrow
ints) are rejected at the call site.

## Indexing

List indexing (`list[i]` and `list[i] = val`) is *typed* through the native `index` / `index_mut` methods but *lowers* to dedicated IR ops (`IndexGet` / `IndexSet` → `INDEX_GET_LIST` / `INDEX_SET_LIST`), not a native call.

`index` is typed `fun List<T>.index(idx: i32): borrowed T` — the `borrowed` modifier demotes the element type to a borrow so `index` yields a *view*, not a transfer. For `List<uniq Point>` the result is `ref Point`, so `var x: uniq Point = list[i]` is a `ref → uniq` type error (you can't move an element out from under the list; borrow it or `pop()` it). For copyable `T` (`List<i32>`) `borrowed T` is just `T`, so indexing copies as before. The modifier is native-signature-only — it is not spellable in user source. See [lifetimes.md → The `borrowed` type modifier](lifetimes.md#the-borrowed-type-modifier).

An out-of-bounds `list[i]` **read** throws a catchable `IndexError` via a cheap
in-IR bounds check before the element read. See [exceptions.md → Index-operator
exceptions](exceptions.md#index-operator-exceptions). `.pop()` and the
`inout`/`out` element-borrow lvalue path still abort on an empty/out-of-range
access.

## Copy and Move Semantics

A `List<T>` is **always noncopyable**, whatever `T` is: it owns a heap element buffer, so — like `uniq` — it is move-only, with the same move / use-after-move rules ([lifetimes.md](lifetimes.md)). `.copy()` produces an independent duplicate when one is genuinely wanted (element-wise; rejected when `T` isn't copyable).

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

At scope exit the compiler emits a single typed `Delete` whose drop plan (`DropKind::List`, element plan nested) drops each element that needs it, then frees the buffer and header. See
[lifetimes.md → One derivation, two executions](lifetimes.md#one-derivation-two-executions).

## Files

`rt/roxy_rt.{h,cpp}` hold the shared implementation; `vm/list.{hpp,cpp}` is a thin VM shim (preserves the VM's nullptr+error contract on bounds violations); registration is in `vm/natives.cpp`.

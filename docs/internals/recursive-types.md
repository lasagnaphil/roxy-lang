# Recursive Types

Roxy supports recursive (self-referential) value types — linked lists, trees, tagged-union ASTs, and mutually recursive structs.

## Background

Structs are value types laid out sequentially in memory (slot-based, no padding). A struct that embeds another struct embeds it directly, so a direct value-type cycle — `struct Node { value: i32; next: Node; }` — has infinite size and is rejected at compile time.

`uniq T` fields break the cycle: they are pointer-sized (2 slots = 8 bytes) regardless of `T`'s layout. `uniq` is also nullable — `nil` can be assigned to `uniq` variables and fields — providing the natural base case for recursion.

Three things make recursive types work:

1. **Self-reference resolution.** The semantic analyzer registers struct names in Pass 1, so a field of type `uniq Node` inside `Node` resolves in Pass 2. `get_type_slot_count()` returns 2 for any `uniq T` without resolving `T`'s full layout, which breaks the recursion in slot-count computation. Mutually recursive structs (A contains `uniq B`, B contains `uniq A`) resolve the same way.
2. **Cycle detection.** A direct value-type cycle without indirection produces a clear "infinite size" error instead of looping forever.
3. **Recursive destruction.** Cleanup of recursive ownership chains is descriptor-driven, so deep chains destroy without overflowing the native stack.

## Syntax

No new syntax — recursive types use existing `uniq` and `nil`:

```roxy
struct Node {
    value: i32;
    next: uniq Node;    // nullable owned pointer to another Node
}

fun main(): i32 {
    var list: uniq Node = uniq Node {
        value = 1,
        next = uniq Node { value = 2, next = uniq Node { value = 3, next = nil } }
    };
    if (list.next != nil) {
        print(f"{list.next.value}");   // 2
    }
    return 0;
}
```

## Patterns

### Linked List

```roxy
struct ListNode {
    value: i32;
    next: uniq ListNode;
}

fun list_push(node: ref ListNode, value: i32) {
    var current: ref ListNode = node;
    while (current.next != nil) {
        current = current.next;
    }
    current.next = uniq ListNode { value = value, next = nil };
}
```

### Binary Tree

```roxy
struct TreeNode {
    value: i32;
    left: uniq TreeNode;
    right: uniq TreeNode;
}

fun tree_sum(node: ref TreeNode): i32 {
    var sum: i32 = node.value;
    if (node.left != nil) { sum = sum + tree_sum(node.left); }
    if (node.right != nil) { sum = sum + tree_sum(node.right); }
    return sum;
}
```

### AST (Tagged Union + Recursion)

```roxy
enum ExprKind { Literal, Negate, Add }

struct Expr {
    when kind: ExprKind {
        case Literal: value: i32;
        case Negate:  operand: uniq Expr;
        case Add:     left: uniq Expr; right: uniq Expr;
    }
}

fun eval(e: ref Expr): i32 {
    when e.kind {
        case Literal: return e.value;
        case Negate:  return -eval(e.operand);
        case Add:     return eval(e.left) + eval(e.right);
    }
}
```

### Mutually Recursive Types

```roxy
struct Forest {
    trees: List<uniq Tree>;
}

struct Tree {
    value: i32;
    children: Forest;
}
```

`Forest` contains `List<uniq Tree>` (heap pointers) and `Tree` contains `Forest` (value-embedded). Since `List<uniq Tree>` stores pointers, both sizes are finite: `Forest` is 2 slots (its `List`), and `Tree` is 1 (value) + 2 (Forest's List) = 3 slots.

## Cycle Detection

During `resolve_type_members`, the analyzer maintains a set of struct types currently being resolved. When a struct field is itself a directly-embedded struct already in that set, it reports an infinite-size error. `uniq T` / `ref T` / `weak T` fields skip the check — they are always pointer-sized.

```
error: recursive struct type 'Node' has infinite size
  --> main.roxy:1:1
  |
1 | struct Node { value: i32; next: Node; }
  |                                 ^^^^
  hint: use 'uniq Node' for indirection
```

## Recursive Destruction

When a `uniq` owner goes out of scope, its destructor runs and the object is freed; for a recursive structure this cascades through owned `uniq` fields until `nil` is reached.

Originally each node re-entered the bytecode interpreter to run its destructor (`interpret()` → `call_cleanup_destructor` → `delete_value` → `interpret()` …), pushing a full interpreter stack frame per ownership level — a 500-node linked list overflowed the native stack.

**Descriptor-driven cleanup.** Parentless structs with a synthetic (compiler-generated) default destructor encode their owned-field cleanup as data: a `BCDeleteDesc` of kind `STRUCT_FIELDS` / `STRUCT_FIELDS+DEL_OBJ` listing each owned field as a `(slot_offset, field_desc)` action, with discriminant-guarded actions for tagged-union (`when`-clause) variant fields. The runtime walks these fields directly in C++ (`delete_value`, `vm/interpreter.cpp`) instead of running a bytecode destructor, exactly as `List`/`Map` element cleanup does. The descriptor is built once per type and memoized (`m_delete_desc_cache` in `lowering.cpp`) with reservation-before-recursion, so a self-referential struct yields a finite, self-referencing descriptor.

This removes the heavyweight `interpret()` re-entry per node: destruction now recurses only through small `delete_value` frames, raising the practical depth limit by ~100× (deep linked lists destroy cleanly into the tens of thousands of nodes). Structs with a **user-defined** destructor, or that use **inheritance**, keep the original bytecode-destructor path — their bodies must run via the interpreter, and inherited-field cleanup chains through parent destructors.

**Remaining limit.** `delete_value` is still recursive in C++, so a sufficiently deep chain (hundreds of thousands of nodes) can still overflow. A fully bounded fix would make `delete_value` iterative via an explicit work-stack; this is deliberately deferred as it is not needed in practice.

### Reassigning a `uniq` field

Assigning to a `uniq` field that already holds a value (`node.next = uniq Node { ... }`) deletes the old value first — recursively cleaning up the old subtree, with a null check that skips deletion when the field is `nil` — then stores the new pointer. This is the same behavior as any other `uniq` field reassignment.

## Files

| File | Purpose |
|---|---|
| `src/roxy/compiler/semantic.cpp` | Self-reference resolution, direct value-cycle detection |
| `src/roxy/compiler/lowering.cpp` | `BCDeleteDesc` construction, `m_delete_desc_cache` memoization |
| `include/roxy/compiler/lowering.hpp` | `BCDeleteDesc` definition |
| `include/roxy/vm/bytecode.hpp` | `BCDeleteDesc` kinds (`STRUCT_FIELDS`, `STRUCT_FIELDS+DEL_OBJ`) |
| `src/roxy/vm/interpreter.cpp` | `delete_value` descriptor-driven cleanup |
| `tests/e2e/test_recursive_types.cpp` | E2E tests |

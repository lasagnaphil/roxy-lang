# Recursive Types

Roxy supports recursive (self-referential) value types — linked lists, trees, tagged-union ASTs, and mutually recursive structs.

## Background

Structs are value types laid out sequentially in memory (slot-based, no padding). A struct that embeds another struct embeds it directly, so a direct value-type cycle — `struct Node { value: i32; next: Node; }` — has infinite size and is rejected at compile time.

`uniq T` fields break the cycle: they are pointer-sized (2 slots = 8 bytes) regardless of `T`'s layout. `uniq` is also nullable — `nil` can be assigned to `uniq` variables and fields — providing the natural base case for recursion.

Self-reference resolves because struct names are registered before members, and `get_type_slot_count()` returns 2 for any `uniq T` without resolving `T`'s layout. Mutually recursive structs (A contains `uniq B`, B contains `uniq A`) resolve the same way.

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

Struct member resolution is memoized and recursive (`ensure_struct_members_resolved` + `StructTypeInfo::members_resolved`): a value-embedded struct field, parent struct, tagged-union variant field, or generic-instance field pulls in the referenced struct's resolution on demand. Declaration order therefore doesn't matter — forward references to later-declared structs are legal.

The analyzer maintains a set of struct types currently being resolved (`m_resolving_structs`). When resolution re-enters a struct already in that set, the embedding is a genuine value-type cycle — direct, mutual, or through a generic instance — and it reports an infinite-size error at the embedding field. `uniq T` / `ref T` / `weak T` fields skip both the recursion and the check — they are always pointer-sized.

```
error: recursive struct type 'Node' has infinite size; use 'uniq Node' for indirection
  --> main.roxy:1:1
  |
1 | struct Node { value: i32; next: Node; }
  |                                 ^^^^
```

## Recursive Destruction

When a `uniq` owner goes out of scope, its destructor runs and the object is freed; for a recursive structure this cascades through owned `uniq` fields until `nil` is reached.

Re-entering the bytecode interpreter per node to run its destructor pushes a full interpreter frame per ownership level (a 500-node list overflowed the native stack), so cleanup is **descriptor-driven** where possible: parentless structs with a synthetic (compiler-generated) default destructor encode their owned-field cleanup as data — a `BCDeleteDesc` with `WalkFields` cleanup listing each owned field as a `(slot_offset, field_desc)` action, with discriminant-guarded actions for tagged-union variant fields. The runtime walks these directly in C++ (`delete_value`, `vm/interpreter.cpp`), exactly as `List`/`Map` element cleanup does. Descriptors are memoized per type in `lowering.cpp` with reservation-before-recursion, so a self-referential struct yields a finite, self-referencing descriptor. Deep lists destroy cleanly into the tens of thousands of nodes.

Structs with a **user-defined** destructor, or that use **inheritance**, keep the bytecode-destructor path — their bodies must run via the interpreter, and inherited-field cleanup chains through parent destructors.

**Remaining limit.** `delete_value` is still recursive in C++, so a sufficiently deep chain (hundreds of thousands of nodes) can still overflow. A fully bounded fix would make `delete_value` iterative via an explicit work-stack; this is deliberately deferred as it is not needed in practice.

### Reassigning a `uniq` field

Assigning to a `uniq` field that already holds a value (`node.next = uniq Node { ... }`) deletes the old value first — recursively cleaning up the old subtree, with a null check that skips deletion when the field is `nil` — then stores the new pointer.

**Tests:** `tests/e2e/test_recursive_types.cpp`

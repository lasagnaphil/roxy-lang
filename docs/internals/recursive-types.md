# Recursive Types

This document describes the design for supporting recursive (self-referential) value types in Roxy.

## Current State

Roxy structs are value types laid out sequentially in memory (slot-based, no padding). A struct containing another struct embeds it directly. This makes recursive types impossible without indirection — `struct Node { value: i32; next: Node; }` would have infinite size.

`uniq T` fields provide pointer indirection (2 slots = 8 bytes, just a pointer) and can break the size cycle. Additionally, `uniq` is already nullable — `nil` can be assigned to `uniq` variables and fields, providing a natural base case for recursion.

However, recursive types are not yet supported because:

1. **Type resolution may reject self-references.** The semantic analyzer resolves struct fields in Pass 2; a struct referencing itself via `uniq` may not be handled correctly depending on resolution order.
2. **No cycle detection for direct embedding.** If a user writes `struct Node { next: Node; }` (without `uniq`), the layout computation may loop infinitely instead of producing a clear error.
3. **Recursive destruction untested.** Cleanup of recursive ownership chains (e.g., linked list nodes) needs to work correctly with the existing destructor/cleanup mechanism.

## Goals

1. Enable self-referential structs via `uniq` indirection (linked lists, trees, ASTs)
2. Enable mutually recursive structs (A contains `uniq B`, B contains `uniq A`)
3. Detect and reject direct value-type cycles with clear error messages
4. Handle recursive destruction safely

## Syntax

No new syntax is needed. Recursive types use existing `uniq` and `nil`:

```roxy
struct Node {
    value: i32;
    next: uniq Node;    // nullable owned pointer to another Node
}

fun main(): i32 {
    // Create a linked list: 1 -> 2 -> 3 -> nil
    var list: uniq Node = uniq Node {
        value = 1,
        next = uniq Node {
            value = 2,
            next = uniq Node {
                value = 3,
                next = nil
            }
        }
    };

    // Check for nil before accessing
    if (list.next != nil) {
        print(f"{list.next.value}");   // 2
    }
    return 0;
}
```

## Common Patterns

### Linked List

```roxy
struct ListNode {
    value: i32;
    next: uniq ListNode;
}

fun list_len(node: ref ListNode): i32 {
    var count: i32 = 1;
    var current: ref ListNode = node;
    while (current.next != nil) {
        current = current.next;
        count = count + 1;
    }
    return count;
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
    if (node.left != nil) {
        sum = sum + tree_sum(node.left);
    }
    if (node.right != nil) {
        sum = sum + tree_sum(node.right);
    }
    return sum;
}
```

### AST (Tagged Union + Recursion)

```roxy
enum ExprKind { Literal, Negate, Add }

struct Expr {
    when kind: ExprKind {
        case Literal:
            value: i32;
        case Negate:
            operand: uniq Expr;
        case Add:
            left: uniq Expr;
            right: uniq Expr;
    }
}

fun eval(e: ref Expr): i32 {
    when e.kind {
        case Literal: return e.value;
        case Negate: return -eval(e.operand);
        case Add: return eval(e.left) + eval(e.right);
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

Here `Forest` contains `List<uniq Tree>` (heap-allocated pointers) and `Tree` contains `Forest` (value-embedded). Since `List<uniq Tree>` stores pointers, the sizes are finite:
- `Forest.slot_count` = `List` slot count (2 slots, pointer to heap array)
- `Tree.slot_count` = 1 (value) + 2 (Forest's List) = 3 slots

## Implementation Changes

### Self-Reference Resolution

During semantic analysis Pass 2 (`resolve_type_members`), when a struct field references its own type via `uniq`:

1. The struct name is already registered in Pass 1 (type exists but fields unresolved)
2. When resolving a field of type `uniq Node` where `Node` is the current struct, the type lookup succeeds (Pass 1 registered it)
3. The slot count for `uniq Node` is always 2 (pointer), regardless of whether `Node`'s own fields are fully resolved
4. No cycle in slot count computation — indirection breaks the recursion

**Verification needed:** Confirm that the current implementation of `get_type_slot_count()` doesn't try to recursively resolve the struct's full layout when computing the slot count of a `uniq` field pointing to the same struct. Since `uniq T` always returns 2 slots regardless of `T`, this should work, but needs testing.

### Cycle Detection for Direct Embedding

Add a check for direct value-type cycles (struct A embeds struct B embeds struct A without indirection). This produces a clear error instead of infinite recursion:

```
error: recursive struct type 'Node' has infinite size
  --> main.roxy:1:1
  |
1 | struct Node { value: i32; next: Node; }
  |                                 ^^^^
  hint: use 'uniq Node' for indirection
```

**Implementation:** During `resolve_type_members`, maintain a set of struct types currently being resolved. When computing the slot count for a struct field:
- If the field type is a struct that is in the "currently resolving" set → cycle error
- `uniq T`, `ref T`, `weak T` fields skip this check (they are always pointer-sized)

```cpp
// In resolve_type_members:
m_resolving_structs.insert(type);  // mark as in-progress

for (auto& field : fields) {
    if (field.type->kind == TypeKind::Struct) {
        if (m_resolving_structs.contains(field.type)) {
            error_fmt(field.loc,
                "recursive struct type '%s' has infinite size; "
                "use 'uniq %s' for indirection",
                field.type->struct_info.name,
                field.type->struct_info.name);
        }
    }
    // uniq/ref/weak fields are fine - always pointer-sized
}

m_resolving_structs.erase(type);  // done resolving
```

### Recursive Destruction

When a `uniq` owner goes out of scope, its destructor runs and then the object is freed. For recursive structures, this happens naturally via the existing cleanup mechanism:

1. `Node` goes out of scope → delete Node object
2. Node's destructor (auto-generated) cleans up owned fields
3. `next: uniq Node` field → if non-null, delete the next Node
4. Recursion continues until `nil` is reached

**Stack overflow risk:** Very deep recursive structures (e.g., a linked list with 100,000 nodes) could overflow the C++ call stack during destruction. Mitigation options:

- **Accept the limit** for v1 (most practical structures are trees, not 100K-deep chains)
- **Iterative cleanup** for tail-position owned fields: the compiler can detect when a struct's last owned field is `uniq Self` and generate an iterative loop instead of recursive destruction
- **Trampoline-based cleanup** in the VM for deeply nested structures

For v1, accept the limit and document it. Iterative cleanup can be added later as an optimization.

### Assigning to `uniq` Fields on Recursive Structs

When assigning to a `uniq` field that already has a value:

```roxy
node.next = uniq Node { value = 5, next = nil };
```

The compiler must:
1. Delete the old value (if non-null) — recursively cleans up the old subtree
2. Store the new pointer

This is the same as the existing behavior for `uniq` field reassignment. The null check before deletion is important: if the field is `nil`, skip deletion.

## Implementation Plan

### Step 1: Cycle Detection
- Add direct value-type cycle detection in `resolve_type_members`
- Maintain a "currently resolving" set during struct layout computation
- Produce clear error messages with hints about using `uniq` for indirection

### Step 2: Self-Referential Struct Resolution
- Verify that Pass 1/Pass 2 ordering handles self-references via `uniq`
- Verify `get_type_slot_count()` returns 2 for `uniq T` without resolving `T`'s full layout
- Verify mutually recursive structs resolve correctly
- Fix any issues found

### Step 3: Recursive Destruction
- Verify auto-generated destructors handle `uniq` fields that point to the same struct type
- Ensure null check before deletion for fields that may be `nil`
- Test destruction of linked lists and trees

### Step 4: Testing
- Self-referential struct via `uniq` (linked list: create, traverse, destroy)
- Binary tree with `uniq` children (create, recursive operations, destroy)
- Tagged union with `uniq` recursive fields (AST: create, eval, destroy)
- Mutually recursive structs (Forest/Tree)
- Nil assignment to recursive field (deletes subtree)
- Reassignment of recursive field (deletes old, assigns new)
- Direct value-type cycle error (compile-time)
- Deep recursive destruction (verify reasonable depth works)

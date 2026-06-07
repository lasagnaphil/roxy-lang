# Recursive Types

This document describes how Roxy supports recursive (self-referential) value types.

## Background

Roxy structs are value types laid out sequentially in memory (slot-based, no padding). A struct containing another struct embeds it directly, so a direct value-type cycle — `struct Node { value: i32; next: Node; }` — would have infinite size and is rejected at compile time.

`uniq T` fields provide pointer indirection (2 slots = 8 bytes, just a pointer) and break the size cycle. Additionally, `uniq` is nullable — `nil` can be assigned to `uniq` variables and fields, providing a natural base case for recursion.

Recursive types are fully supported. Three pieces make this work, each detailed below:

1. **Self-reference resolution.** The semantic analyzer registers struct names in Pass 1, so a field of type `uniq Node` inside `Node` resolves correctly in Pass 2 — `uniq T` is always pointer-sized regardless of `T`'s layout.
2. **Cycle detection for direct embedding.** A direct value-type cycle (`struct Node { next: Node; }`, no indirection) produces a clear "infinite size" error instead of looping forever.
3. **Recursive destruction.** Cleanup of recursive ownership chains (linked lists, trees) is descriptor-driven, so deep chains destroy without overflowing the native stack.

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

`get_type_slot_count()` does not recursively resolve a struct's full layout when computing the slot count of a `uniq` field pointing to the same struct — `uniq T` always returns 2 slots regardless of `T`, which is what breaks the recursion. This is covered by the E2E tests listed below.

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

**Stack overflow risk:** Very deep recursive structures could overflow the C++ call stack during destruction. Originally, destroying each node re-entered the bytecode interpreter (`interpret()` → `call_cleanup_destructor` → `delete_value` → `interpret()` …), so a *full interpreter stack frame* was pushed per ownership level — a 500-node linked list overflowed the stack.

**Descriptor-driven struct cleanup (implemented):** Parentless structs with a synthetic (compiler-generated) default destructor now have their owned-field cleanup encoded as data — a `BCDeleteDesc` of kind `STRUCT_FIELDS` (`5`) / `STRUCT_FIELDS+DEL_OBJ` (`6`) that lists each owned field as a `(slot_offset, field_desc)` action, with discriminant-guarded actions for tagged-union (`when`-clause) variant fields. The runtime walks these fields directly in C++ (`delete_value`, `vm/interpreter.cpp`) instead of running a bytecode destructor, exactly as `List`/`Map` element cleanup already did. The descriptor is built once per type and memoized (`m_delete_desc_cache` in `lowering.cpp`) with reservation-before-recursion, so a self-referential struct (`next: uniq Node`) yields a finite, self-referencing descriptor.

This removes the heavyweight `interpret()` re-entry per node: destruction now recurses only through small `delete_value` frames, raising the practical depth limit by ~100× (a deep linked list now destroys cleanly into the tens of thousands of nodes). Structs with a **user-defined** destructor, or that use **inheritance**, keep the original bytecode-destructor path (their bodies must run via the interpreter, and inherited-field cleanup chains through parent destructors).

**Remaining limit:** `delete_value` is still recursive in C++, so a sufficiently deep chain (hundreds of thousands of nodes) can still overflow. A fully bounded fix would make `delete_value` iterative via an explicit work-stack (push children instead of recursing); this was deliberately deferred as it was not needed in practice.

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

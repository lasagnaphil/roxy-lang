# Tagged Unions (Discriminated Unions)

> **Status:** Core implementation complete. Flow-sensitive typing and
> exhaustiveness checking are not yet implemented.

Tagged unions let a struct hold variant-specific fields selected by a discriminant
enum value. They give memory-efficient sum types with a union layout (all variants
share storage) and pattern matching via the `when` statement. The feature reuses
the existing struct, enum, and field-access machinery — there is no dedicated
runtime representation.

## Struct Definition with `when`

A `when` clause inside a struct declares a **discriminant field** (an enum) and
**case blocks** of variant-specific fields:

```roxy
enum SkillType { Attack, Defend }

struct Skill {
    name_id: i32;          // fixed field

    when type: SkillType {
        case Attack:
            damage: i32;
        case Defend:
            damage_reduce: i32;
    }
}
```

Here `type` is the discriminant and `damage` / `damage_reduce` are the per-variant
fields. Fixed fields may appear before and after the `when` clause.

## Struct Literals

A tagged-union struct is constructed by setting the discriminant explicitly, then
the fields for that variant. The compiler validates that the supplied variant
fields match the discriminant value.

```roxy
var skill: Skill = Skill {
    name_id = 1,
    type = SkillType::Attack,
    damage = 100
};
```

> Variant-constructor syntax (`Skill.Attack { ... }`) is **not implemented**.

## Pattern Matching with `when`

The `when` statement matches a discriminant expression and unlocks that variant's
fields inside the corresponding `case`:

```roxy
fun use_skill(skill: ref Skill) {
    when skill.type {
        case Attack:
            print(f"{skill.damage}");          // skill.damage accessible here
        case Defend:
            print(f"{skill.damage_reduce}");   // skill.damage_reduce here
    }
}
```

Matching is **partial** — unhandled cases fall through as no-ops. An optional
`else` block handles the default:

```roxy
when skill.type {
    case Attack:
        print(f"{skill.damage}");
    else:
        print("not an attack");
}
```

Variables modified inside cases are merged at the end of the `when` via phi
(block-argument) values, so assignments made in a case are visible after it.

### Grouped cases

Multiple case names share a body. Only fields common to all grouped variants are
accessible (typically none):

```roxy
when spell.element {
    case Fire, Ice, Lightning:
        return true;
    case Earth:
        return false;
}
```

Cases may also nest (`when` inside a `case` body).

## Memory Layout

All variants of a `when` clause share one union region sized to the largest
variant. A struct lays out as: fixed fields, then for each `when` clause a
discriminant slot followed by the union storage.

```roxy
struct AttackData { damage: i32; crit_chance: f32; }  // 2 slots
struct DefendData { damage_reduce: f32; }              // 1 slot

struct Skill {
    name: string;           // 2 slots (pointer)
    hit_chance: f32;        // 1 slot
    when type: SkillType {  // 1 slot discriminant + 2 slots union
        case Attack: attack: AttackData;   // 2 slots
        case Defend: defend: DefendData;   // 1 slot
    }
}
// Total: 2 + 1 + 1 + 2 = 6 slots
```

```
┌─────────────────────────────────────┐
│ Slot 0-1: name (string ptr)         │ Fixed fields
│ Slot 2:   hit_chance (f32)          │
├─────────────────────────────────────┤
│ Slot 3:   type (SkillType/i32)      │ Discriminant
├─────────────────────────────────────┤
│ Slot 4-5: UNION                     │ Variant data
│   if Attack: damage, crit_chance    │
│   if Defend: damage_reduce, <pad>   │
└─────────────────────────────────────┘
```

All variant fields share the same union base offset; each variant's fields are
laid out sequentially from that base (so `Attack.damage` and `Defend.damage_reduce`
both occupy union slot 0).

### Multiple `when` clauses

A struct may have several independent `when` clauses, each contributing its own
discriminant slot and union region. Each is matched separately.

```roxy
struct Ability {
    name: string;
    when damage_type: DamageType {
        case Physical: armor_penetration: f32;
        case Magical:  magic_scaling: f32;
    }
    when target_type: TargetType {
        case Single: range: f32;
        case Area:   radius: f32;
    }
}
```

```
┌────────────────────────────────┐
│ name (string)                  │
│ damage_type (DamageType)       │
│ UNION 1 (damage variants)      │
│ target_type (TargetType)       │
│ UNION 2 (target variants)      │
└────────────────────────────────┘
```

## Type Safety Rules

Variant field access is gated at compile time, so no runtime discriminant checks
are emitted.

**Variant fields require a matching `when` case.** Accessing a variant field
outside its case is a compile error:

```roxy
fun example(skill: ref Skill) {
    // var x = skill.damage;        // ERROR: outside a 'when'
    when skill.type {
        case Attack: var x = skill.damage;          // OK
        case Defend: var y = skill.damage_reduce;   // OK
    }
}
```

**The discriminant and fixed fields are always accessible.**

```roxy
fun check(skill: ref Skill): bool {
    return skill.type == SkillType::Attack;  // discriminant always readable
}
```

**Constructors unlock variant fields after setting the discriminant** to a
compile-time-constant value:

```roxy
fun new Skill.make_attack(dmg: i32) {
    self.name_id = 1;
    self.type = SkillType::Attack;   // after this...
    self.damage = dmg;               // ...this variant field is valid
}
```

## Interaction with Other Features

- **Inheritance** — child structs inherit a parent's `when` clauses and may add
  fixed fields. They cannot add new cases to an inherited clause, since that would
  change the union size.
- **Methods** — pattern-match on `self.<discriminant>` normally.
- **Destructors** — a `fun delete` should handle every variant; owned (`uniq`)
  fields inside a variant are cleaned up under their matching case.
- **`out`/`inout` parameters** — work as with any struct; mutate variant fields
  inside the matching case.
- **Lists** — `List<Skill>` stores tagged unions inline; pattern-match per element.

## Implementation

`when` statements lower to a comparison chain (`==` against each enum value plus a
conditional branch) rather than a dedicated `SWITCH` opcode — for the small enums
typical of game scripting the comparison chain is competitive and simpler. Variant
field accesses become `GetField` at the union base offset. Variables assigned
across cases are reconciled at a merge block via phi (block-argument) values.

The semantic analyzer validates that the discriminant is an enum, that case names
are variants of *that* enum (resolved through the enum type's own variant table,
so a same-named variant of a different enum is rejected), and that struct-literal
variant fields match the discriminant. It computes union layout (max of variant
slot counts) and per-variant field offsets.

## Grammar

```
// struct definition
when_clause     -> "when" Identifier ":" type_expr "{"
                   case_field+
                   "}" ;
case_field      -> "case" Identifier ( "," Identifier )* ":" field_decl* ;

// when statement
when_stmt       -> "when" expression "{"
                   when_case+
                   ( "else" ":" statement* )?
                   "}" ;
when_case       -> "case" Identifier ( "," Identifier )* ":" statement* ;
```

## Files

| File | Purpose |
|---|---|
| `include/roxy/compiler/ast.hpp` | `WhenFieldDecl` / `CaseFieldDecl` (struct decls), `StmtWhen` / `WhenCase` |
| `include/roxy/compiler/types.hpp` | `WhenClauseInfo`, `VariantInfo`; `StructTypeInfo::when_clauses`, variant field lookup |
| `src/roxy/compiler/parser.cpp` | parse `when` clauses and `when` statements |
| `src/roxy/compiler/semantic.cpp` | `resolve_when_clauses`, union layout, variant-access and struct-literal validation |
| `src/roxy/compiler/ir_builder.cpp` | `gen_when_stmt` (comparison chain + phi merge), variant field access |
| `src/roxy/compiler/lowering.cpp` | union memory access in bytecode |
| `tests/e2e/test_tagged_unions.cpp` | E2E tests |

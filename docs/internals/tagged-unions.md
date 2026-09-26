# Tagged Unions (Discriminated Unions)

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

> **Exhaustiveness detection.** When the cases cover *every* variant of the
> discriminant enum, the `when` is **exhaustive** (`WhenStmt::is_exhaustive`):
> (1) if every arm returns/throws, the `when` terminates, so no trailing return
> is required; (2) a `uniq` moved in every arm is `Moved`, not `MaybeValid`,
> afterwards; (3) the impossible fall-through becomes an `Unreachable` trap.
> This is detection, not enforcement: a non-exhaustive `when` is not an error
> (partial matching is intentional).

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
discriminant slot and union region (laid out in declaration order). Each is
matched separately.

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

## Type Safety Rules

Variant field access is **checked at runtime**, not at compile time. Every read
or write of a variant field loads the discriminant and compares it against that
variant's value; on a mismatch the program traps with *"variant field access
with wrong discriminant"* (`TRAP` in the VM). A `when` arm, an `if` on the
discriminant, or code that has just assigned the discriminant all pass the check
naturally — none of them is required by the compiler:

```roxy
fun example(skill: ref Skill) {
    when skill.type {
        case Attack: var x = skill.damage;          // OK
        case Defend: var y = skill.damage_reduce;   // OK
    }
    if (skill.type == SkillType::Attack) {
        var z = skill.damage;                       // OK — guarded by the if
    }
    var w = skill.damage;   // compiles; traps at runtime unless type == Attack
}
```

Flow-sensitive typing that would reject the unguarded access at compile time is
a planned feature (`TODO.md` → Planned Features).

**The discriminant and fixed fields are always accessible.**

```roxy
fun check(skill: ref Skill): bool {
    return skill.type == SkillType::Attack;  // discriminant always readable
}
```

**Constructors set the discriminant before its variant fields**, so the check
passes:

```roxy
fun new Skill.make_attack(dmg: i32) {
    self.name_id = 1;
    self.type = SkillType::Attack;   // after this...
    self.damage = dmg;               // ...this variant field passes the check
}
```

## Interaction with Other Features

- **Inheritance** — `when` clauses are not inherited: a child struct cannot access
  its parent's variant fields (`struct 'Big' has no field 'damage'`). See
  [inheritance.md](inheritance.md).
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
field accesses become `GetField` at the union base offset.

Case names resolve through the discriminant enum's own variant table, so a
same-named variant of a *different* enum is rejected.

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

Layout and validation: `src/roxy/compiler/sema/semantic.cpp` (`resolve_when_clauses`); `when` lowering: `gen_when_stmt` in `src/roxy/compiler/ir/ir_builder_stmt.cpp`; variant access + discriminant check: `src/roxy/compiler/ir/ir_builder_expr.cpp`. Tests: `tests/e2e/test_tagged_unions.cpp`.

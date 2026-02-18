# Tagged Unions (Discriminated Unions)

> **Status:** Core implementation complete (2026-02-01). Flow-sensitive typing and exhaustiveness checking are not yet implemented.

Tagged unions allow structs to contain variant-specific fields that depend on a discriminant value. This provides memory-efficient sum types with compile-time safety.

## Syntax Overview

### Struct Definition with `when`

```roxy
enum SkillType { Attack, Defend }

struct AttackData {
    damage: i32;
    crit_chance: f32;
}

struct DefendData {
    damage_reduce: f32;
}

struct Skill {
    name: string;
    hit_chance: f32;

    when type: SkillType {
    case Attack:
        attack: AttackData;
    case Defend:
        defend: DefendData;
    }
}
```

The `when` clause declares:
- A **discriminant field** (`type: SkillType`) that determines the active variant
- **Case blocks** with variant-specific fields

### Pattern Matching with `when` Statement

```roxy
fun use_skill(skill: ref Skill) {
    when skill.type {
        case Attack:
            // skill.attack is accessible here
            var dmg: i32 = skill.attack.damage;
            print(f"{dmg}");
        case Defend:
            // skill.defend is accessible here
            var reduce: f32 = skill.defend.damage_reduce;
            print_f32(reduce);
    }
}
```

### Struct Literals with Variants

**Explicit discriminant syntax:**
```roxy
var skill: Skill = Skill {
    name = "Fireball",
    hit_chance = 0.9,
    type = Attack,
    attack = AttackData { damage = 50, crit_chance = 0.15 }
};
```

The discriminant field (`type`) is set explicitly, and the compiler validates that the variant fields match the discriminant value.

## Memory Layout

Tagged unions use a **union layout** where all variants share the same memory region. The struct contains:

1. **Fixed fields** (before the `when` clause)
2. **Discriminant field** (the enum value)
3. **Union storage** (size = maximum of all variant sizes)
4. **Fixed fields** (after the `when` clause, if any)

### Example Layout

```roxy
struct AttackData { damage: i32; crit_chance: f32; }  // 2 slots (8 bytes)
struct DefendData { damage_reduce: f32; }              // 1 slot (4 bytes)

struct Skill {
    name: string;           // 2 slots (pointer)
    hit_chance: f32;        // 1 slot
    when type: SkillType {  // 1 slot (discriminant) + 2 slots (union)
        case Attack:
            attack: AttackData;   // 2 slots
        case Defend:
            defend: DefendData;   // 1 slot
    }
}
// Total: 2 + 1 + 1 + 2 = 6 slots (24 bytes)
```

Memory visualization:
```
┌─────────────────────────────────────┐
│ Slot 0-1: name (string ptr)         │ Fixed fields
│ Slot 2:   hit_chance (f32)          │
├─────────────────────────────────────┤
│ Slot 3:   type (SkillType/i32)      │ Discriminant
├─────────────────────────────────────┤
│ Slot 4-5: UNION                     │ Variant data
│   if Attack: attack.damage (i32)    │
│              attack.crit_chance     │
│   if Defend: defend.damage_reduce   │
│              <padding>              │
└─────────────────────────────────────┘
```

### Union Field Offsets

All variant fields start at the same base offset (slot 4 in the example). Each variant's fields are laid out sequentially from that base:

| Variant | Field | Offset from union base |
|---------|-------|------------------------|
| Attack | attack.damage | slot 0 |
| Attack | attack.crit_chance | slot 1 |
| Defend | defend.damage_reduce | slot 0 |

## Type Safety Rules

### Rule 1: Variant fields require pattern matching

Variant-specific fields can **only** be accessed inside a matching `when` case:

```roxy
fun example(skill: ref Skill) {
    // ERROR: skill.attack is not accessible outside 'when'
    // var x = skill.attack.damage;

    when skill.type {
        case Attack:
            var x = skill.attack.damage;  // OK
        case Defend:
            var x = skill.defend.damage_reduce;  // OK
    }
}
```

### Rule 2: Discriminant is always readable

```roxy
fun check_type(skill: ref Skill): bool {
    return skill.type == Attack;  // OK - discriminant is always accessible
}
```

### Rule 3: Fixed fields are always accessible

```roxy
fun get_name(skill: ref Skill): string {
    return skill.name;  // OK - fixed field
}
```

### Rule 4: Constructors can set variant fields after discriminant

Inside constructors, setting the discriminant to a compile-time constant unlocks the corresponding variant fields:

```roxy
fun new Skill.make_attack(name: string, dmg: i32) {
    self.name = name;
    self.hit_chance = 0.9;
    self.type = Attack;  // After this line...
    self.attack = AttackData { damage = dmg, crit_chance = 0.1 };  // ...this is valid
}
```

## Pattern Matching Details

### Partial Matching

`when` statements allow partial matching - unhandled cases are implicit no-ops:

```roxy
when skill.type {
    case Attack:
        print(f"{skill.attack.damage}");
    // Defend case is implicitly skipped
}
```

Use `else` for explicit default handling:

```roxy
when skill.type {
    case Attack:
        print(f"{skill.attack.damage}");
    else:
        print("Not an attack");
}
```

**Note:** The compiler may emit warnings for unhandled cases in debug mode, but it's not an error.

### Multiple cases

Group cases that share handling:

```roxy
enum Element { Fire, Ice, Lightning, Earth }

struct Spell {
    when element: Element {
        case Fire:
            burn_duration: i32;
        case Ice:
            freeze_chance: f32;
        case Lightning:
            chain_count: i32;
        case Earth:
            armor_bonus: i32;
    }
}

fun is_elemental_damage(spell: ref Spell): bool {
    when spell.element {
        case Fire, Ice, Lightning:
            return true;
        case Earth:
            return false;
    }
}
```

**Note:** When grouping cases, only fields common to ALL grouped cases are accessible (which is typically none for tagged unions).

### Nested pattern matching

```roxy
when outer.type {
    case TypeA:
        when outer.inner.subtype {
            case SubType1:
                // ...
        }
}
```

### When as expression (future)

```roxy
var damage: i32 = when skill.type {
    case Attack: skill.attack.damage
    case Defend: 0
};
```

## Variant Constructors

Each case in a `when` clause generates an implicit **variant constructor**:

```roxy
struct Skill {
    name: string;
    when type: SkillType {
        case Attack: attack: AttackData;
        case Defend: defend: DefendData;
    }
}

// These are implicitly available:
// Skill.Attack { name, attack }
// Skill.Defend { name, defend }
```

Variant constructors:
- Set the discriminant automatically
- Require the variant-specific fields
- Require all fixed fields (or use defaults)

### Variant constructor with defaults

```roxy
struct Config {
    debug: bool = false;
    when mode: Mode {
        case Fast: optimization_level: i32 = 3;
        case Safe: bounds_check: bool = true;
    }
}

// Minimal construction:
var c1: Config = Config.Fast { };  // debug=false, optimization_level=3
var c2: Config = Config.Safe { bounds_check = false };  // debug=false
```

## Multiple `when` Clauses

A struct can have multiple independent `when` clauses:

```roxy
enum DamageType { Physical, Magical }
enum TargetType { Single, Area }

struct Ability {
    name: string;

    when damage_type: DamageType {
        case Physical:
            armor_penetration: f32;
        case Magical:
            magic_scaling: f32;
    }

    when target_type: TargetType {
        case Single:
            range: f32;
        case Area:
            radius: f32;
    }
}
```

Each `when` clause is independent - you must pattern match on each discriminant separately:

```roxy
fun describe(ability: ref Ability) {
    when ability.damage_type {
        case Physical:
            print("Physical, penetration: ");
            print_f32(ability.armor_penetration);
        case Magical:
            print("Magical, scaling: ");
            print_f32(ability.magic_scaling);
    }

    when ability.target_type {
        case Single:
            print("Range: ");
            print_f32(ability.range);
        case Area:
            print("Radius: ");
            print_f32(ability.radius);
    }
}
```

Memory layout with multiple unions:
```
┌────────────────────────────────┐
│ name (string)                  │
│ damage_type (DamageType)       │
│ UNION 1 (damage variants)      │
│ target_type (TargetType)       │
│ UNION 2 (target variants)      │
└────────────────────────────────┘
```

## Interaction with Other Features

### Inheritance

Child structs inherit the parent's `when` clauses:

```roxy
struct Skill {
    name: string;
    when type: SkillType {
        case Attack: attack: AttackData;
        case Defend: defend: DefendData;
    }
}

struct SpecialSkill : Skill {
    cooldown: i32;  // Additional fixed field
}

// SpecialSkill has: name, type, attack/defend union, cooldown
```

**Restriction:** Child structs cannot add new cases to inherited `when` clauses (this would change the union size).

### Methods

Methods can use pattern matching normally:

```roxy
fun Skill.get_damage(): i32 {
    when self.type {
        case Attack:
            return self.attack.damage;
        case Defend:
            return 0;
    }
}

fun Skill.apply_crit(): i32 {
    when self.type {
        case Attack:
            if (rand_f32() < self.attack.crit_chance) {
                return self.attack.damage * 2;
            }
            return self.attack.damage;
        case Defend:
            return 0;
    }
}
```

### Constructors and Destructors

Named constructors typically create a specific variant:

```roxy
fun new Skill.fireball() {
    self.name = "Fireball";
    self.type = Attack;
    self.attack = AttackData { damage = 100, crit_chance = 0.2 };
}

fun new Skill.shield() {
    self.name = "Shield";
    self.type = Defend;
    self.defend = DefendData { damage_reduce = 0.5 };
}
```

Destructors should handle all variants:

```roxy
fun delete Skill() {
    when self.type {
        case Attack:
            // Cleanup attack-specific resources if any
        case Defend:
            // Cleanup defend-specific resources if any
    }
}
```

### Out/Inout Parameters

Tagged unions work with parameter modifiers:

```roxy
fun upgrade_attack(skill: inout Skill) {
    when skill.type {
        case Attack:
            skill.attack.damage = skill.attack.damage + 10;
        case Defend:
            // Can't upgrade - maybe convert to attack?
    }
}
```

### Arrays of Tagged Unions

```roxy
var skills: List<Skill> = List<Skill>();
skills.push(Skill.Attack { name = "Slash", attack = AttackData { damage = 20 } });
skills.push(Skill.Defend { name = "Block", defend = DefendData { damage_reduce = 0.3 } });

for (var i: i32 = 0; i < skills.len(); i = i + 1) {
    when skills[i].type {
        case Attack:
            print(f"{skills[i].attack.damage}");
        case Defend:
            print_f32(skills[i].defend.damage_reduce);
    }
}
```

## Runtime Behavior

### No Runtime Checks by Default

Since variant field access requires compile-time pattern matching, no runtime checks are needed. The compiler guarantees that:
- Inside `case Attack:`, only `attack` fields are accessed
- Inside `case Defend:`, only `defend` fields are accessed

### Debug Mode Validation (Optional)

In debug builds, the VM could optionally validate that the discriminant matches when accessing variant fields. This catches bugs from unsafe memory manipulation.

## Grammar

```
// In struct definition
when_clause     -> "when" Identifier ":" type_expr "{"
                   case_field+
                   "}" ;

case_field      -> "case" Identifier ( "," Identifier )* ":"
                   field_decl* ;

// When statement
when_stmt       -> "when" expression "{"
                   when_case+
                   ( "else" ":" statement* )?
                   "}" ;

when_case       -> "case" Identifier ( "," Identifier )* ":"
                   statement* ;

// Struct literal with variant
struct_literal  -> Identifier ( "." Identifier )? "{" field_init_list? "}" ;
```

## Type System Additions

### WhenClause

```cpp
struct WhenClause {
    StringView discriminant_name;     // "type"
    Type* discriminant_type;          // Enum type
    u32 discriminant_slot_offset;     // Where discriminant is stored
    u32 union_slot_offset;            // Where union data starts
    u32 union_slot_count;             // Size of union (max of all variants)
    Span<VariantInfo> variants;       // One per case
};
```

### VariantInfo

```cpp
struct VariantInfo {
    StringView case_name;             // "Attack"
    i64 discriminant_value;           // Enum value (0, 1, ...)
    Span<FieldInfo> fields;           // Fields for this variant
    u32 variant_slot_count;           // Size of this variant
};
```

### StructTypeInfo Changes

```cpp
struct StructTypeInfo {
    StringView name;
    Decl* decl;
    Type* parent;                     // For inheritance
    Span<FieldInfo> fields;           // Fixed fields
    Span<MethodInfo> methods;
    Span<WhenClause> when_clauses;    // NEW: Tagged union clauses
    u32 slot_count;                   // Total size including unions
};
```

## AST Additions

### WhenFieldDecl

```cpp
struct WhenFieldDecl {
    StringView discriminant_name;
    TypeExpr* discriminant_type;
    Span<CaseFieldDecl> cases;
    SourceLocation loc;
};
```

### CaseFieldDecl

```cpp
struct CaseFieldDecl {
    Span<StringView> case_names;      // Can have multiple: "case A, B:"
    Span<FieldDecl> fields;
    SourceLocation loc;
};
```

### StmtWhen

```cpp
struct StmtWhen {
    Expr* discriminant_expr;          // The expression being matched
    Span<WhenCase> cases;
    Stmt* else_body;                  // Optional default case
    SourceLocation loc;
};

struct WhenCase {
    Span<StringView> case_names;
    Span<Stmt*> body;
    SourceLocation loc;
};
```

## Semantic Analysis

### Validation Rules

1. **Discriminant must be enum type**
   ```
   error: when discriminant must be an enum type
     --> main.roxy:5:10
      |
      | when count: i32 {
      |      ^^^^^ 'i32' is not an enum type
   ```

2. **Cases must be valid enum variants**
   ```
   error: unknown enum variant
     --> main.roxy:7:10
      |
      | case Flying:
      |      ^^^^^^ 'Flying' is not a variant of 'SkillType'
   ```

3. **Cases must be exhaustive (or have else)**
   ```
   error: non-exhaustive pattern match
     --> main.roxy:10:5
      |
      | when skill.type {
      |      ^^^^^^^^^^ missing case: 'Defend'
   ```

4. **Variant field access requires pattern match**
   ```
   error: cannot access variant field outside pattern match
     --> main.roxy:15:5
      |
      | skill.attack.damage
      | ^^^^^^^^^^^^ 'attack' is only accessible inside 'case Attack:'
   ```

5. **No duplicate cases**
   ```
   error: duplicate case in pattern match
     --> main.roxy:8:10
      |
      | case Attack:
      |      ^^^^^^ 'Attack' already matched at line 6
   ```

### Flow-Sensitive Typing

The semantic analyzer tracks **variant constraints** for each variable:

```cpp
struct VariantConstraint {
    Symbol* variable;                 // The tagged union variable
    StringView when_field;            // Which when clause
    i64 active_variant;               // Which case we're in (-1 = unknown)
};
```

When entering a `case` block:
1. Add constraint: `variable.discriminant == case_value`
2. Variant fields for that case become accessible
3. On exit, remove the constraint

## IR Changes

### Pattern Matching

Pattern matching compiles to a switch-like structure:

```
// when skill.type { case Attack: ... case Defend: ... }

  %disc = GetField skill, discriminant_offset
  Switch %disc, [0 -> bb_attack, 1 -> bb_defend]

bb_attack:
  // skill.attack accessible here
  %dmg = GetField skill, union_offset + 0  // attack.damage
  ...
  Jump bb_end

bb_defend:
  // skill.defend accessible here
  %reduce = GetField skill, union_offset + 0  // defend.damage_reduce
  ...
  Jump bb_end

bb_end:
  ...
```

### New IR Operation: Switch

```cpp
struct IROp_Switch {
    ValueId discriminant;
    Span<SwitchCase> cases;
    BlockId default_block;            // For else clause
};

struct SwitchCase {
    i64 value;
    BlockId target;
};
```

## Bytecode Changes

### New Opcode: SWITCH

```
SWITCH reg, case_count
  [value0, offset0]
  [value1, offset1]
  ...
  [default_offset]
```

Alternatively, compile to a series of comparisons and jumps (simpler but slower):

```
  LOAD_INT r1, 0              // Attack = 0
  EQ_I r2, r0, r1             // r0 = discriminant
  JMP_IF r2, offset_attack
  LOAD_INT r1, 1              // Defend = 1
  EQ_I r2, r0, r1
  JMP_IF r2, offset_defend
  JMP offset_else
```

For small enums (2-4 variants), the comparison chain is likely faster. For larger enums, a jump table is better.

## Implementation Order

### Phase 1: Parser Changes
- [x] Parse `when` clause in struct definitions
- [x] Parse `when` statement for pattern matching
- [x] New AST nodes: `WhenFieldDecl`, `WhenCaseFieldDecl`, `WhenStmt`, `WhenCase`
- [ ] Parse variant constructor syntax: `Type.Variant { ... }`

### Phase 2: Type System Changes
- [x] Add `WhenClauseInfo`, `VariantInfo`, `VariantFieldInfo` to type system
- [x] Extend `StructTypeInfo` with `when_clauses`
- [x] Calculate union layout (max of variant sizes)
- [x] Variant field offset calculation

### Phase 3: Semantic Analysis
- [x] Validate `when` clauses (enum discriminant, valid cases)
- [ ] Check case exhaustiveness
- [ ] Implement flow-sensitive typing for variant field access
- [x] Validate struct literals with variants
- [x] Constructor variant field access rules

### Phase 4: IR Builder
- [x] Generate IR for `when` statements (comparison chain)
- [x] Handle variant field access with correct offsets
- [x] Phi node support for variable modifications across case branches
- [ ] Variant constructor code generation

### Phase 5: Bytecode & VM
- [x] Uses comparison chain (SWITCH opcode not needed for small enums)
- [x] Union memory access working

### Phase 6: Testing
- [x] Parser tests for new syntax
- [x] E2E tests for runtime behavior (8 test cases in `tagged_unions_test.cpp`)
- [ ] Edge cases: nested when, multiple when clauses, inheritance

## Examples

### Complete Example: Game Entities

```roxy
enum EntityType { Player, Enemy, NPC }

struct PlayerData {
    level: i32;
    experience: i64;
    inventory_size: i32;
}

struct EnemyData {
    damage: i32;
    aggro_range: f32;
    loot_table_id: i32;
}

struct NPCData {
    dialog_id: i32;
    shop_id: i32;
}

struct Entity {
    id: i32;
    name: string;
    x: f32;
    y: f32;
    hp: i32;
    max_hp: i32;

    when entity_type: EntityType {
        case Player:
            player: PlayerData;
        case Enemy:
            enemy: EnemyData;
        case NPC:
            npc: NPCData;
    }
}

fun new Entity.create_player(name: string, x: f32, y: f32) {
    self.id = next_entity_id();
    self.name = name;
    self.x = x;
    self.y = y;
    self.hp = 100;
    self.max_hp = 100;
    self.entity_type = Player;
    self.player = PlayerData { level = 1, experience = 0l, inventory_size = 20 };
}

fun Entity.update(dt: f32) {
    when self.entity_type {
        case Player:
            // Player-specific update
            if (self.player.experience >= self.player.level * 1000l) {
                self.player.level = self.player.level + 1;
                self.max_hp = self.max_hp + 10;
            }
        case Enemy:
            // Enemy AI
            var dist: f32 = distance_to_player(self.x, self.y);
            if (dist < self.enemy.aggro_range) {
                move_toward_player(self);
            }
        case NPC:
            // NPC idle behavior
    }
}

fun Entity.interact(other: ref Entity) {
    when self.entity_type {
        case Player:
            when other.entity_type {
                case Enemy:
                    // Combat
                    other.hp = other.hp - 10;
                case NPC:
                    // Open dialog
                    open_dialog(other.npc.dialog_id);
                else:
                    // Player-player interaction
            }
        case Enemy:
            // Enemies don't initiate interaction
        case NPC:
            // NPCs don't initiate interaction
    }
}

fun main(): i32 {
    var player: Entity = Entity.create_player("Hero", 0.0, 0.0);

    var goblin: Entity = Entity.Enemy {
        id = 1,
        name = "Goblin",
        x = 10.0,
        y = 5.0,
        hp = 30,
        max_hp = 30,
        enemy = EnemyData { damage = 5, aggro_range = 15.0, loot_table_id = 1 }
    };

    player.interact(goblin);

    return 0;
}
```

## Comparison with Alternatives

| Approach | Memory | Safety | Syntax | Implementation |
|----------|--------|--------|--------|----------------|
| Tagged unions (this design) | Optimal (union) | Compile-time | Clean, integrated | Medium |
| Rust-style enums | Optimal (union) | Compile-time | Separate enum | Medium |
| Manual union + tag | Optimal | None | Verbose | Already possible |
| Inheritance + heap | Wasteful | Runtime | OOP-style | Already implemented |

Tagged unions provide the best balance of memory efficiency, safety, and syntax for Roxy's game scripting use case.

## Dependencies

Implementing tagged unions requires:

1. **Enum implementation** - ✓ Already complete
2. **Struct field access** - ✓ Already complete
3. **Flow-sensitive typing** - New capability needed
4. **Switch IR/bytecode** - New or use comparison chain

## Files to Modify

| File | Changes |
|------|---------|
| `include/roxy/compiler/ast.hpp` | Add WhenFieldDecl, CaseFieldDecl, StmtWhen, WhenCase |
| `include/roxy/compiler/types.hpp` | Add WhenClause, VariantInfo; extend StructTypeInfo |
| `src/roxy/compiler/parser.cpp` | Parse when clause and when statement |
| `src/roxy/compiler/semantic.cpp` | Validate when clauses, flow-sensitive typing |
| `src/roxy/compiler/ir_builder.cpp` | Generate switch/comparison IR |
| `src/roxy/compiler/lowering.cpp` | Lower switch to bytecode |
| `include/roxy/vm/bytecode.hpp` | Add SWITCH opcode (optional) |
| `src/roxy/vm/interpreter.cpp` | Implement SWITCH (optional) |
| `tests/unit/*.cpp` | Parser, semantic tests |
| `tests/e2e/tagged_unions_test.cpp` | E2E tests |
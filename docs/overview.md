# Roxy Language

> **Note:** This document is a design document and roadmap for the Roxy language.
> Many features described here are planned but not yet implemented.
> For the current language grammar, see [grammar.md](grammar.md).

## How does it look like?

```kotlin
import randutils;
import stdio;
enum SkillType {
    Attack, Defend
}

struct AttackSkill {
    damage: int
    crit_chance: float
    crit_multiplier: float
}

struct DefendSkill {
    damage_reduce_multiplier: float
}

struct Skill {
    str: string
    hit_chance: float
    when type: SkillType {
        case Attack:
            attack: AttackSkill
        case Defend:
            defend: DefendSkill
    }
}

struct Entity {
    hp: int
}

struct Player : Entity {
    mp: int
    damage_reduce_multiplier: float
    skills: List<uniq Skill>
    skill_map: Map<string, ref Skill>
}

struct Monster : Entity {
    damage: int
}

fun new Player.default_character() { // custom constructor
    super.new(hp = 100);
    
    damage_reduce_multiplier = 0.0;
    
    add_skill(Skill {
        str = "Regular Attack", hit_chance = 0.90,
        type = SkillType.Attack {
            attack = AttackSkill { damage = 10, crit_chance = 0.10 }
        }
    });

    add_skill(Skill {
        str = "Regular Defend", hit_chance = 0.95,
        type = SkillType.Defend {
            defend = DefendSkill { damage*reduce*multiplier = 0.50 }
        }
    });
}

fun Player.add_skill(skill: Skill) {
    skills.push(skill);
    skill_map[skill.str] = skill;
}

fun Player.get_skill_by_name(str: string): Skill? {
    // ref Skill -> weak Skill automatic conversion
    return skill_map.get_or_default(str, null);
}

// Custom destructor, need to call this manually (unless default destructor is not defined)
fun delete Player.remove_from_game() {
    print("Remove player from game!")
}

fun new Monster.dummy() {
    super.new(hp = 30);
    damage = 10;
}

// When there is no custom destructor, default destructor is automatically synthesized

fun play_turn(player: ref Player, monster: ref Monster, usedskill: Skill) {
    var orig_damage_reduce_multplier = player.damage_reduce_multiplier;
    
    // Player uses skill
    when skill.type {
        case Attack:
            if throw_coin(skill.hit_chance) {
                var damage = skill.attack.damage;
                if throw_coin(skill.attack.crit_chance) {
                    damage *= skill.attack.crit_multiplier;
                }
                monster.hp -= damage;
            }
            else {
                print("Miss!")
            }
        case Defend:
            if throw_coin(skill.hit_chance) {
                player.damage_reduce_multplier = clamp(
                    player.damage_reduce_multiplier + skill.defend.damage_reduce_multiplier, 0, 1);
            }
    }

    // Monster attacks
    player.hp -= monster.damage;

    // Resets player stat
    player.damage_reduce_multiplier = orig_damage_reduce_multiplier;
}

fun main(args: List<string>) {
    var player = uniq new Player.default_character();
    var special_skill = Skill {
        str = "Regular Attack", hit_chance = 0.90,
        type = SkillType.Attack {
            attack = AttackSkill { damage = 10, crit_chance = 0.10 }
        }
    };
    
    player.add_skill(special_skill);

    var monster = uniq new Monster.dummy();
    while true {
        var input = stdio.input();
        if input == "quit" {
            break;
        }
        else {
            var skill = player.get_skill_by_name(input);
            if (skill) {
                play_turn(player, monster, skill);
            }
        }
    }
    
    delete player.remove_from_game();
}

```

## Features

### Static typing by default

- Static typing is just much better for IDE support

- Creates much better optimization opportunities by the compiler

- Static typing works better with value semantics (reduce boxing)

- Static typing makes the compiler/VM more complex and slow?

    - Devs tend to add gradual typing to their dynamic-typed languages anyway...

    - We should do this at the beginning instead of retrofitting it in the middle of language development

### Value semantics by default

- Reduce excessive boxing which is typical in managed languages

    - Reduce alloc/dealloc costs

    - Better cache locality (ex. arrays and hashmaps can now store values contiguously)

    - Less pressure on the refcount system

- Enables fast interop with C / C++ structs

- In / out references for value objects

    - Out reference (out): write-only (out parameters in C#)

    - Inout reference (inout): the combination of the two (ref parameter in C#)

    - Note that these references cannot be converted to uniq / ref / weak in any way!

    - There is no 'in' reference, just pass by value instead!

        - The value will be copied only if it overwritten inside the function, or small enough (<= 16 bytes)

- Capturing by closure always copies the value instead of referencing it

    - Removes confusion ("the dreaded for-loop dillema")

    - Simpler language design (no need for a complex reference system)

    - If you really want to share a value with a closure, box the value and take a strong / weak reference instead!

### No garbage collection, memory management with unique / strong / weak references

- Three kinds of "smart pointers" for boxed objects, for convenience and safety:

    - Unique reference (uniq): Owns the object, can be new / deleted

    - Strong reference (ref): References the object, asserts when a dangling reference is produced

        - Basically a borrow checker done at runtime!

        - Uses ref-counting under the hood

        - Compiler can optimize out some inc-ref / dec-refs with static analysis

    - Weak reference (weak): References the object, asserts when a dangling reference is used

        - Provides much faster weak pointers than C++!

        - Uses random generational uids under the hood

        - The only reference type that can be 'null'

- Auto-dereference by default (no -> as in C++)

- Auto-conversion rules (simple to remember!):

    - value -> uniq, value -> inout, value -> out

    - uniq -> ref, uniq -> weak

    - ref -> weak

    - inout -> out

- In total, the system is memory-safe without the drawbacks of garbage collection or conventional ref-counted smart-pointers!

### Convenient and safe init / deinit via named constructors / destructors

- Unlike C++, you can specify multiple versions of constructors / destructors

    - Default constructor / destructor: is automatically run, use RAII semantics

    - Custom constructor / destructor: called manually

    - If default constructor / destructor is not specified, usage of value before constructor / after destructor is caught at compile-time!

    - Compiler does branch analysis to ensure that destructor is called at all times during the current scope

- Values are always initialized to zero by default

### Superb C/C++ embedding API, fast interop with C / C++ structs

- An easy-to-use C++ API for binding external code using templates

- Also has a (less ergonomic) C API for people wanting to bind the interpreter with other languages

- Most importantly: C/C++ structs do not have to be boxed, and can be exposed to the language directly!

- Instead there are some limits imposed on the C/C++ struct:

    - Fields need to be 8-byte aligned

    - For safety, use the types provided by the C API (Integer, Double, etc...)

    - For C++, the struct needs to be POD (since the system uses the C ABI)

### IDE support baked in the compiler

- The compiler is built from day-1 for LSP server support (like how Roslyn was made)

- Parses the source code from scratch each keystroke, but with a smart caching system that reuses AST fragments

- Aiming for fast incremental compilation times with this approach to compiler design

### Fast compilation times

- For the bytecode interpreter, will sacrifice some runtime performance to gain compile times / startup times
    - Aiming for faster performance than reference Lua but slower than Java / C#
    - In most cases, binding layer performance should be much more important than raw performance

- In the future, there might be a "transpile to C" option for people who really need the "close-to-metal" performance
    - JITs are nice but they are too complex...
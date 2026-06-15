#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Map Tests
// ============================================================================

TEST_SUITE("E2E Maps") {

    TEST_CASE_TEMPLATE("Map basic insert and get", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            m.insert(3, 30);
            print(f"{m.get(1)}");
            print(f"{m.get(2)}");
            print(f"{m.get(3)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n20\n30\n");
    }

    TEST_CASE_TEMPLATE("Map len tracking", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            print(f"{m.len()}");
            m.insert(1, 10);
            print(f"{m.len()}");
            m.insert(2, 20);
            print(f"{m.len()}");
            m.insert(3, 30);
            print(f"{m.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "0\n1\n2\n3\n");
    }

    TEST_CASE_TEMPLATE("Map index operator read and write", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m[1] = 100;
            m[2] = 200;
            print(f"{m[1]}");
            print(f"{m[2]}");
            // Overwrite via indexing
            m[1] = 999;
            print(f"{m[1]}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "100\n200\n999\n");
    }

    TEST_CASE_TEMPLATE("Map contains", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(42, 1);
            if (m.contains(42)) {
                print("yes");
            }
            if (!m.contains(99)) {
                print("no");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "yes\nno\n");
    }

    TEST_CASE_TEMPLATE("Map remove", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            var removed: bool = m.remove(1);
            print(f"{removed}");
            print(f"{m.len()}");
            if (!m.contains(1)) {
                print("gone");
            }
            // Remove non-existent key
            var not_removed: bool = m.remove(99);
            print(f"{not_removed}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\ntrue\n1\ngone\nfalse\n");
    }

    TEST_CASE_TEMPLATE("Map clear", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            m.clear();
            print(f"{m.len()}");
            if (!m.contains(1)) {
                print("empty");
            }
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n0\nempty\n");
    }

    TEST_CASE_TEMPLATE("Map string keys", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<string, i32> = Map<string, i32>();
            var key1: string = "hello";
            var key2: string = "world";
            m.insert(key1, 1);
            m.insert(key2, 2);
            print(f"{m.get(key1)}");
            print(f"{m.get(key2)}");
            print(f"{m.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "1\n2\n2\n");
    }

    TEST_CASE_TEMPLATE("Map i32 keys with string values", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, string> = Map<i32, string>();
            m.insert(1, "one");
            m.insert(2, "two");
            print(m.get(1));
            print(m.get(2));
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "one\ntwo\n");
    }

    TEST_CASE_TEMPLATE("Map overwrite existing key", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            print(f"{m.get(1)}");
            m.insert(1, 99);
            print(f"{m.get(1)}");
            print(f"{m.len()}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "10\n99\n1\n");
    }

    TEST_CASE_TEMPLATE("Map growth and rehashing", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            var i: i32 = 0;
            while (i < 50) {
                m.insert(i, i * 10);
                i = i + 1;
            }
            print(f"{m.len()}");
            print(f"{m.get(0)}");
            print(f"{m.get(25)}");
            print(f"{m.get(49)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "50\n0\n250\n490\n");
    }

    TEST_CASE_TEMPLATE("Map with initial capacity", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>(64);
            m.insert(1, 10);
            m.insert(2, 20);
            print(f"{m.len()}");
            print(f"{m.get(1)}");
            print(f"{m.get(2)}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n10\n20\n");
    }

    TEST_CASE("Map missing key runtime error") {  // VM-only: runtime-trap/abort behavior differs on C backend (VM-only by nature)
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(1, 10);
            var x: i32 = m.get(99);
            return x;
        }
    )";

        auto result = VMBackend::run(source);
        CHECK_FALSE(result.success);
    }

    TEST_CASE_TEMPLATE("Map keys and values", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        fun main(): i32 {
            var m: Map<i32, i32> = Map<i32, i32>();
            m.insert(10, 100);
            m.insert(20, 200);
            var k: List<i32> = m.keys();
            var v: List<i32> = m.values();
            print(f"{k.len()}");
            print(f"{v.len()}");
            var key_sum: i32 = k[0] + k[1];
            var val_sum: i32 = v[0] + v[1];
            print(f"{key_sum}");
            print(f"{val_sum}");
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "2\n2\n30\n300\n");
    }

    // ============================================================================
    // Map with struct-typed values (regression: Map<K, V> used to store a dangling
    // pointer to the caller's local stack for V larger than 8 bytes; as soon as
    // any subsequent method call reused the same local-stack region, the value
    // bytes decoded by get() were whatever the second frame had just written.)
    // ============================================================================

    TEST_CASE("Map<string, Struct>: value survives subsequent method call with local struct") {  // VM-only: C backend: struct-valued Map persistence gap
        const char* source = R"ROXY(
        struct Val { pub a: i32; pub b: i32; pub c: i32; }
        fun make_val(): Val { return Val { a = 43690, b = 48059, c = 52428 }; }

        struct Env {
            pub values: Map<string, Val>;
            pub depth: i32;
        }
        fun new Env(d: i32) {
            self.values = Map<string, Val>();
            self.depth = d;
        }

        struct Interp { pub envs: List<Env>; }
        fun new Interp() {
            self.envs = List<Env>(256);
            self.envs.push(Env(-1));
        }
        fun Interp.define(k: string, v: Val) {
            self.envs[0].values.insert(k, v);
        }
        fun Interp.touch() {
            var e: Env = Env(0);
        }

        fun main(): i32 {
            var i: Interp = Interp();
            i.define("x", make_val());
            i.touch();
            var v: Val = i.envs[0].values.get("x");
            if (v.a == 43690 && v.b == 48059 && v.c == 52428) {
                return 1;
            }
            return 0;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE("Map<string, Struct>: value preserved across List.push to enclosing list") {  // VM-only: C backend: struct-valued Map persistence gap
        // Original TODO-report pattern: method A inserts into a nested map,
        // method B pushes to the enclosing list. Struct-valued map entries must
        // stay valid across the list push.
        const char* source = R"ROXY(
        struct Val { pub a: i32; pub b: i32; pub c: i32; }
        fun make_val(): Val { return Val { a = 1, b = 2, c = 3 }; }

        struct Env {
            pub values: Map<string, Val>;
            pub depth: i32;
        }
        fun new Env(d: i32) {
            self.values = Map<string, Val>();
            self.depth = d;
        }

        struct Interp { pub envs: List<Env>; }
        fun new Interp() {
            self.envs = List<Env>(256);
            self.envs.push(Env(-1));
        }
        fun Interp.define(k: string, v: Val) {
            self.envs[0].values.insert(k, v);
        }
        fun Interp.push_block() {
            self.envs.push(Env(0));
        }

        fun main(): i32 {
            var i: Interp = Interp();
            i.define("x", make_val());
            i.push_block();
            var v: Val = i.envs[0].values.get("x");
            return v.a + v.b + v.c;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE("Map<i32, Struct>: rehash preserves struct values") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        // Exercises map_grow + map_insert_internal's Robin Hood swap on
        // variable-sized values. Insert enough entries to trigger at least one
        // grow, then verify every value is intact.
        const char* source = R"ROXY(
        struct Val { pub x: i32; pub y: i32; pub z: i32; }

        fun main(): i32 {
            var m: Map<i32, Val> = Map<i32, Val>();
            var i: i32 = 0;
            while (i < 50) {
                m.insert(i, Val { x = i, y = i * 2, z = i * 3 });
                i = i + 1;
            }
            var sum: i32 = 0;
            var j: i32 = 0;
            while (j < 50) {
                var v: Val = m.get(j);
                sum = sum + v.x + v.y + v.z;
                j = j + 1;
            }
            return sum;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // sum over j in [0,50) of (j + 2j + 3j) = 6 * sum(0..49) = 6 * 1225 = 7350
        CHECK(result.value == 7350);
    }

    TEST_CASE("Map<i32, Struct>: remove keeps other entries intact") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        // Exercises backward-shift deletion with variable-sized values.
        const char* source = R"ROXY(
        struct Val { pub x: i32; pub y: i32; pub z: i32; }

        fun main(): i32 {
            var m: Map<i32, Val> = Map<i32, Val>();
            m.insert(1, Val { x = 10, y = 20, z = 30 });
            m.insert(2, Val { x = 40, y = 50, z = 60 });
            m.insert(3, Val { x = 70, y = 80, z = 90 });
            m.remove(2);
            var v1: Val = m.get(1);
            var v3: Val = m.get(3);
            return v1.x + v1.y + v1.z + v3.x + v3.y + v3.z;
        }
    )ROXY";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        // 10+20+30 + 70+80+90 = 300
        CHECK(result.value == 300);
    }

    // ============================================================================
    // Struct keys (MapKeyKind::Struct, bytewise hash + memcmp).
    // Note: Roxy structs are slot-aligned with no compiler padding, so bytewise
    // equality is well-defined for POD struct keys.
    // ============================================================================

    TEST_CASE_TEMPLATE("Map<Struct, i32>: basic insert + get", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Point { x: i32; y: i32; }
        fun main(): i32 {
            var m: Map<Point, i32> = Map<Point, i32>();
            m.insert(Point { x = 1, y = 2 }, 10);
            m.insert(Point { x = 3, y = 4 }, 32);
            return m.get(Point { x = 1, y = 2 }) + m.get(Point { x = 3, y = 4 });
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE_TEMPLATE("Map<Struct, i32>: contains, remove, len", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Pair { a: i32; b: i32; }
        fun main(): i32 {
            var m: Map<Pair, i32> = Map<Pair, i32>();
            m.insert(Pair { a = 1, b = 1 }, 100);
            m.insert(Pair { a = 2, b = 2 }, 200);
            m.insert(Pair { a = 3, b = 3 }, 300);
            var len_before: i32 = m.len();
            var has_two: bool = m.contains(Pair { a = 2, b = 2 });
            var missing: bool = m.contains(Pair { a = 9, b = 9 });
            m.remove(Pair { a = 2, b = 2 });
            var len_after: i32 = m.len();
            var bits: i32 = 0;
            if (has_two) bits = bits + 1;
            if (!missing) bits = bits + 2;
            return len_before * 10 + len_after + bits;
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        // len_before=3, len_after=2, has_two=1, missing=0 → 30 + 2 + 3 = 35
        CHECK(result.value == 35);
    }

    TEST_CASE_TEMPLATE("Map<Struct, Struct>: both sides struct", Backend, RX_E2E_BACKENDS) {
        const char* source = R"ROXY(
        struct Pos { x: i32; y: i32; }
        struct Color { r: i32; g: i32; b: i32; }
        fun main(): i32 {
            var m: Map<Pos, Color> = Map<Pos, Color>();
            m.insert(Pos { x = 0, y = 0 }, Color { r = 1, g = 2, b = 3 });
            m.insert(Pos { x = 1, y = 1 }, Color { r = 10, g = 20, b = 30 });
            var c: Color = m.get(Pos { x = 1, y = 1 });
            return c.r + c.g + c.b;
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 60);
    }

    TEST_CASE("Map<Struct, i32>: rehash with struct keys") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        // Inserts cross the 80% load threshold and force map_grow; this exercises
        // the ping-pong scratch buffers for variable-sized keys.
        const char* source = R"ROXY(
        struct Key { a: i32; b: i32; c: i32; }
        fun main(): i32 {
            var m: Map<Key, i32> = Map<Key, i32>();
            for (var i: i32 = 0; i < 25; i = i + 1) {
                m.insert(Key { a = i, b = i * 2, c = i * 3 }, i * 100);
            }
            return m.get(Key { a = 7, b = 14, c = 21 });
        }
    )ROXY";
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 700);
    }

    // ============================================================================
    // Custom Hash / Eq dispatch for struct keys via runtime callback.
    // The runtime calls the user's `K.hash()` / `K.eq(other)` methods through
    // `call_user_function` (re-entrant interpreter call) when they're defined.
    // Detection is by method-name lookup — no `Eq` builtin trait required.
    // ============================================================================

    TEST_CASE_TEMPLATE("Map<Struct, i32>: custom hash dispatched", Backend, RX_E2E_BACKENDS) {
        // The user's Vec2.hash() is dispatched during insert and get.
        const char* source = R"ROXY(
        struct Vec2 { x: i32; y: i32; }
        fun Vec2.hash(): u64 for Hash {
            return u64(self.x * 31 + self.y);
        }
        fun main(): i32 {
            var m: Map<Vec2, i32> = Map<Vec2, i32>();
            m.insert(Vec2 { x = 1, y = 2 }, 42);
            return m.get(Vec2 { x = 1, y = 2 });
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("Map<Struct, i32>: custom eq collapses bytewise-different keys") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        // Custom Vec2.eq treats (a,b) and (b,a) as equal, so the second insert
        // overwrites the first and the map ends up with one entry.
        const char* source = R"ROXY(
        struct Vec2 { x: i32; y: i32; }
        fun Vec2.hash(): u64 for Hash {
            // Symmetric hash so colliding pairs land in the same bucket.
            return u64(self.x + self.y);
        }
        fun Vec2.eq(other: Vec2): bool for Eq {
            return (self.x == other.x && self.y == other.y) ||
                   (self.x == other.y && self.y == other.x);
        }
        fun main(): i32 {
            var m: Map<Vec2, i32> = Map<Vec2, i32>();
            m.insert(Vec2 { x = 1, y = 2 }, 100);
            m.insert(Vec2 { x = 2, y = 1 }, 200);   // overwrites under custom eq
            var len: i32 = i32(m.len());
            var v: i32 = m.get(Vec2 { x = 1, y = 2 });
            return len * 1000 + v;   // expect 1*1000 + 200 = 1200
        }
    )ROXY";
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1200);
    }

    TEST_CASE("Map<Struct, i32>: only hash defined, eq falls back to bytewise") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        // No user-defined eq → bytewise memcmp. Two distinct-byte keys remain
        // distinct entries even though they collide on hash.
        const char* source = R"ROXY(
        struct K { x: i32; y: i32; }
        fun K.hash(): u64 for Hash {
            return 42ul;  // everything collides
        }
        fun main(): i32 {
            var m: Map<K, i32> = Map<K, i32>();
            m.insert(K { x = 1, y = 2 }, 100);
            m.insert(K { x = 3, y = 4 }, 200);
            return i32(m.len()) * 1000 + m.get(K { x = 3, y = 4 });
        }
    )ROXY";
        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2200);
    }

    TEST_CASE_TEMPLATE("Map<Struct, i32>: hash method without `for Hash` is NOT dispatched", Backend, RX_E2E_BACKENDS) {
        // Just defining `hash()` on a struct doesn't enable custom dispatch —
        // the struct must explicitly `impl Hash`. Without `for Hash`, the runtime
        // falls back to bytewise hash. This means two structs with different
        // bytes are different keys regardless of what the user's hash() returns.
        const char* source = R"ROXY(
        struct Vec2 { x: i32; y: i32; }

        // hash defined but NOT marked `for Hash` — runtime ignores it.
        fun Vec2.hash(): u64 {
            return u64(self.x);  // would collapse different ys if dispatched
        }

        fun main(): i32 {
            var m: Map<Vec2, i32> = Map<Vec2, i32>();
            m.insert(Vec2 { x = 1, y = 1 }, 100);
            m.insert(Vec2 { x = 1, y = 2 }, 200);
            // Bytewise treats these as distinct keys — len = 2.
            return i32(m.len());
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 2);
    }

    TEST_CASE_TEMPLATE("Map<Struct, i32>: custom hash survives rehash", Backend, RX_E2E_BACKENDS) {
        // 30 inserts force map_grow, which rehashes via the user's Hash method
        // (the runtime calls back into user code for each key during rehash).
        const char* source = R"ROXY(
        struct K { a: i32; b: i32; }
        fun K.hash(): u64 for Hash {
            return u64(self.a * 31 + self.b);  // simple mix
        }
        fun K.eq(other: K): bool for Eq {
            return self.a == other.a && self.b == other.b;
        }
        fun main(): i32 {
            var m: Map<K, i32> = Map<K, i32>();
            for (var i: i32 = 0; i < 30; i = i + 1) {
                m.insert(K { a = i, b = i * 7 }, i + 1);
            }
            return m.get(K { a = 17, b = 119 });
        }
    )ROXY";
        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 18);
    }

    // ============================================================================
    // Bucket cleanup for noncopyable keys / values (regression).
    // On map destruction, each occupied bucket's key and value must be deleted at
    // the correct slot base (keys/values + i * *_slot_count) and dispatched by the
    // entry descriptor's free_obj flag. Older code read keys[i] (wrong bucket,
    // 32-bit-truncated) and treated values as pointers, corrupting cleanup of
    // Map<noncopyable-key, V> and Map<K, struct-value-with-owned-fields>.
    // ============================================================================

    TEST_CASE_TEMPLATE("Map<string, value-struct with destructor>: value cleanup", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Item { id: i32; }
        fun delete Item() { print(f"del {self.id}"); }

        fun main(): i32 {
            var m: Map<string, Item> = Map<string, Item>();
            m.insert("a", Item { id = 7 });
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "del 7\n");
    }

    TEST_CASE_TEMPLATE("Map<string, struct with uniq field>: value field cleanup", Backend, RX_E2E_BACKENDS) {
        const char* source = R"(
        struct Inner { tag: i32; }
        fun delete Inner() { print(f"inner {self.tag}"); }
        struct Holder { val: uniq Inner; }

        fun main(): i32 {
            var m: Map<string, Holder> = Map<string, Holder>();
            m.insert("k", Holder { val = uniq Inner { tag = 30 } });
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "inner 30\n");
    }

    TEST_CASE_TEMPLATE("Map<string, value-struct>: all occupied buckets cleaned up", Backend, RX_E2E_BACKENDS) {
        // Order-independent: each destructor prints the same token, so the count of
        // tokens proves every occupied bucket was visited at the right slot base.
        const char* source = R"(
        struct Item { id: i32; }
        fun delete Item() { print("x"); }

        fun main(): i32 {
            var m: Map<string, Item> = Map<string, Item>();
            m.insert("a", Item { id = 1 });
            m.insert("b", Item { id = 2 });
            m.insert("c", Item { id = 3 });
            m.insert("d", Item { id = 4 });
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "x\nx\nx\nx\n");
    }

    TEST_CASE_TEMPLATE("Map<noncopyable struct key, V>: key cleanup", Backend, RX_E2E_BACKENDS) {
        // K is a noncopyable struct key (has a destructor) with custom Hash/Eq.
        // Destroying the map must run each key's destructor with the correct `self`
        // (keys + i * key_slot_count), not the truncated keys[i].
        const char* source = R"(
        struct K { a: i32; }
        fun K.hash(): u64 for Hash { return u64(self.a); }
        fun K.eq(other: K): bool for Eq { return self.a == other.a; }
        fun delete K() { print(f"delkey {self.a}"); }

        fun main(): i32 {
            var m: Map<K, i32> = Map<K, i32>();
            m.insert(K { a = 5 }, 100);
            return 0;
        }
    )";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.stdout_output == "delkey 5\n");
    }

}  // TEST_SUITE("E2E Maps")

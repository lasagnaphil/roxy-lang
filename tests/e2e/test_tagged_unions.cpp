#include "test_helpers.hpp"

#include <roxy/core/doctest/doctest.h>

using namespace rx;

TEST_CASE("E2E - Tagged unions basic definition") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: f32;
            }
        }

        fun main(): i32 {
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 0);
}

TEST_CASE("E2E - Tagged unions struct literal with variant A") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: f32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 42 };
            print(f"{d.val_a}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Tagged unions struct literal with variant B") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::B, val_b = 99 };
            print(f"{d.val_b}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "99\n");
}

TEST_CASE("E2E - Tagged unions with regular fields") {
    const char* source = R"(
        enum SkillType { Attack, Defend }

        struct Skill {
            name_id: i32;
            when type: SkillType {
                case Attack:
                    damage: i32;
                case Defend:
                    damage_reduce: i32;
            }
        }

        fun main(): i32 {
            var skill: Skill = Skill {
                name_id = 1,
                type = SkillType::Attack,
                damage = 100
            };
            print(f"{skill.name_id}");
            print(f"{skill.damage}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "1\n100\n");
}

TEST_CASE("E2E - Tagged unions multiple fields per variant") {
    const char* source = R"(
        enum Kind { Point2D, Point3D }

        struct Point {
            when kind: Kind {
                case Point2D:
                    x: i32;
                    y: i32;
                case Point3D:
                    px: i32;
                    py: i32;
                    pz: i32;
            }
        }

        fun main(): i32 {
            var p: Point = Point { kind = Kind::Point2D, x = 10, y = 20 };
            print(f"{p.x}");
            print(f"{p.y}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

TEST_CASE("E2E - Tagged unions variant field assignment") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 10 };
            d.val_a = 20;
            print(f"{d.val_a}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "20\n");
}

TEST_CASE("E2E - Tagged unions method access") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun Data.get_val_a(): i32 {
            return self.val_a;
        }

        fun main(): i32 {
            var d: Data = Data { kind = Kind::A, val_a = 42 };
            print(f"{d.get_val_a()}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "42\n");
}

TEST_CASE("E2E - Tagged unions use with when statement") {
    const char* source = R"(
        enum Kind { A, B }

        struct Data {
            when kind: Kind {
                case A:
                    val_a: i32;
                case B:
                    val_b: i32;
            }
        }

        fun main(): i32 {
            var d1: Data = Data { kind = Kind::A, val_a = 10 };
            when d1.kind {
                case A:
                    print(f"{d1.val_a}");
                case B:
                    print(f"{d1.val_b}");
            }

            var d2: Data = Data { kind = Kind::B, val_b = 20 };
            when d2.kind {
                case A:
                    print(f"{d2.val_a}");
                case B:
                    print(f"{d2.val_b}");
            }
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n20\n");
}

// ============================================================================
// Tagged unions with uniq fields
// ============================================================================

TEST_CASE("E2E - Tagged unions with uniq field: basic create and access") {
    const char* source = R"(
        struct Leaf {
            value: i32;
        }

        enum NodeKind { LeafNode, Empty }

        struct Node {
            when kind: NodeKind {
                case LeafNode:
                    child: uniq Leaf;
                case Empty:
                    _pad: i32;
            }
        }

        fun main(): i32 {
            var leaf: uniq Leaf = uniq Leaf();
            leaf.value = 42;

            var node: Node = Node { kind = NodeKind::LeafNode, child = leaf };
            var r: ref Leaf = ref node.child;
            return r.value;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Tagged unions with uniq field: RAII cleanup") {
    // The uniq field inside a tagged union variant must be cleaned up
    // when the struct goes out of scope
    const char* source = R"(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"delete Leaf {self.value}");
        }

        enum NodeKind { LeafNode, Empty }

        struct Node {
            when kind: NodeKind {
                case LeafNode:
                    child: uniq Leaf;
                case Empty:
                    _pad: i32;
            }
        }

        fun main(): i32 {
            var leaf: uniq Leaf = uniq Leaf();
            leaf.value = 99;
            var node: Node = Node { kind = NodeKind::LeafNode, child = leaf };
            print("before scope exit");
            return 0;
            // node goes out of scope — child should be deleted
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "before scope exit\ndelete Leaf 99\n");
}

TEST_CASE("E2E - Tagged unions with uniq field: empty variant no crash") {
    // When the active variant has no uniq field, cleanup should be safe
    const char* source = R"(
        struct Leaf {
            value: i32;
        }

        fun delete Leaf() {
            print(f"delete Leaf {self.value}");
        }

        enum NodeKind { LeafNode, Empty }

        struct Node {
            when kind: NodeKind {
                case LeafNode:
                    child: uniq Leaf;
                case Empty:
                    _pad: i32;
            }
        }

        fun main(): i32 {
            var node: Node = Node { kind = NodeKind::Empty, _pad = 0 };
            print("empty variant");
            return 0;
            // node goes out of scope — no uniq to delete
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "empty variant\n");
}

TEST_CASE("E2E - Tagged unions with uniq field: when statement access") {
    const char* source = R"(
        struct Payload {
            data: i32;
        }

        enum MsgKind { Data, None }

        struct Message {
            when kind: MsgKind {
                case Data:
                    payload: uniq Payload;
                case None:
                    _pad: i32;
            }
        }

        fun main(): i32 {
            var p: uniq Payload = uniq Payload();
            p.data = 123;
            var msg: Message = Message { kind = MsgKind::Data, payload = p };

            var result: i32 = 0;
            when msg.kind {
                case Data:
                    var r: ref Payload = ref msg.payload;
                    result = r.data;
                case None:
                    result = -1;
            }
            print(f"{result}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "123\n");
}

TEST_CASE("E2E - Tagged unions recursive: uniq self-reference") {
    // This is the core AST pattern: Expr contains uniq Expr children
    const char* source = R"(
        enum ExprKind { Literal, Binary }

        struct Expr {
            when kind: ExprKind {
                case Literal:
                    value: i32;
                case Binary:
                    left: uniq Expr;
                    right: uniq Expr;
            }
        }

        fun eval(expr: ref Expr): i32 {
            when expr.kind {
                case Literal:
                    return expr.value;
                case Binary:
                    return eval(ref expr.left) + eval(ref expr.right);
            }
            return 0;
        }

        fun main(): i32 {
            var left: uniq Expr = uniq Expr();
            left.kind = ExprKind::Literal;
            left.value = 10;

            var right: uniq Expr = uniq Expr();
            right.kind = ExprKind::Literal;
            right.value = 20;

            var root: uniq Expr = uniq Expr();
            root.kind = ExprKind::Binary;
            root.left = left;
            root.right = right;

            var result: i32 = eval(ref root);
            print(f"{result}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "30\n");
}

TEST_CASE("E2E - Tagged unions recursive: deep tree cleanup") {
    // Verify RAII cleanup for a recursive tree structure
    const char* source = R"(
        enum ExprKind { Literal, Binary }

        struct Expr {
            when kind: ExprKind {
                case Literal:
                    value: i32;
                case Binary:
                    left: uniq Expr;
                    right: uniq Expr;
            }
        }

        fun make_literal(v: i32): uniq Expr {
            var e: uniq Expr = uniq Expr();
            e.kind = ExprKind::Literal;
            e.value = v;
            return e;
        }

        fun make_add(l: uniq Expr, r: uniq Expr): uniq Expr {
            var e: uniq Expr = uniq Expr();
            e.kind = ExprKind::Binary;
            e.left = l;
            e.right = r;
            return e;
        }

        fun eval(expr: ref Expr): i32 {
            when expr.kind {
                case Literal:
                    return expr.value;
                case Binary:
                    return eval(ref expr.left) + eval(ref expr.right);
            }
            return 0;
        }

        fun main(): i32 {
            // Build: (1 + 2) + (3 + 4)
            var tree: uniq Expr = make_add(
                make_add(make_literal(1), make_literal(2)),
                make_add(make_literal(3), make_literal(4))
            );
            var result: i32 = eval(ref tree);
            print(f"{result}");
            return 0;
            // tree and all children cleaned up here
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "10\n");
}

TEST_CASE("E2E - Tagged unions recursive: destructor chaining") {
    // Verify that destroying a uniq tagged union recursively cleans up
    // uniq children in variants. Without a synthetic destructor on Expr,
    // only the root would be freed — children would leak.
    const char* source = R"(
        enum ExprKind { Literal, Binary }

        struct Expr {
            when kind: ExprKind {
                case Literal:
                    value: i32;
                case Binary:
                    left: uniq Expr;
                    right: uniq Expr;
            }
        }

        fun delete Expr() {
            when self.kind {
                case Literal:
                    print(f"delete Literal {self.value}");
                case Binary:
                    print("delete Binary");
            }
        }

        fun main(): i32 {
            var left: uniq Expr = uniq Expr();
            left.kind = ExprKind::Literal;
            left.value = 1;

            var right: uniq Expr = uniq Expr();
            right.kind = ExprKind::Literal;
            right.value = 2;

            var root: uniq Expr = uniq Expr();
            root.kind = ExprKind::Binary;
            root.left = left;
            root.right = right;

            print("before cleanup");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    // All three Expr objects must be destroyed:
    // root (Binary), then its variant fields in LIFO order (right, left)
    CHECK(result.stdout_output == "before cleanup\ndelete Binary\ndelete Literal 2\ndelete Literal 1\n");
}

TEST_CASE("E2E - Tagged unions with List<uniq T> field") {
    const char* source = R"(
        struct Item {
            value: i32;
        }

        enum ContainerKind { Items, Empty }

        struct Container {
            when kind: ContainerKind {
                case Items:
                    items: List<uniq Item>;
                case Empty:
                    _pad: i32;
            }
        }

        fun main(): i32 {
            var i1: uniq Item = uniq Item();
            i1.value = 10;
            var i2: uniq Item = uniq Item();
            i2.value = 20;
            var i3: uniq Item = uniq Item();
            i3.value = 30;

            var items: List<uniq Item> = List<uniq Item>();
            items.push(i1);
            items.push(i2);
            items.push(i3);

            var c: Container = Container { kind = ContainerKind::Items, items = items };

            var sum: i32 = 0;
            var idx: i32 = 0;
            while (idx < c.items.len()) {
                var r: ref Item = ref c.items[idx];
                sum = sum + r.value;
                idx = idx + 1;
            }
            print(f"{sum}");
            return 0;
        }
    )";

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.stdout_output == "60\n");
}

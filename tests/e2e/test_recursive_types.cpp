#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"
#include "test_e2e_backend.hpp"

using namespace rx;

// ============================================================================
// Recursive Types Tests
// ============================================================================

TEST_SUITE("E2E Recursive Types") {

    TEST_CASE("basic uniq self-reference compiles") {
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = nil;
            return n1.value;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module != nullptr);
    }

    TEST_CASE("two-node list") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            return n1.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
    }

    TEST_CASE("access uniq field via ref param") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun get_value(node: ref Node): i32 {
            return node.value;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            return get_value(n1.next);
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 20);
    }

    TEST_CASE("explicit ref expression") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            var r: ref Node = ref n1;
            return r.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 10);
    }

    TEST_CASE("ref of uniq field") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            var r: ref Node = ref n1.next;
            return r.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 20);
    }

    TEST_CASE("linked list traversal with ref") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n3: uniq Node = uniq Node();
            n3.value = 30;
            n3.next = nil;

            var n2: uniq Node = uniq Node();
            n2.value = 20;
            n2.next = n3;

            var n1: uniq Node = uniq Node();
            n1.value = 10;
            n1.next = n2;

            var r: ref Node = ref n1;
            var sum: i32 = r.value;

            r = ref r.next;
            sum = sum + r.value;

            r = ref r.next;
            sum = sum + r.value;

            return sum;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 60);
    }

    TEST_CASE("binary tree sum") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct TreeNode {
            value: i32;
            left: uniq TreeNode;
            right: uniq TreeNode;
        }

        fun tree_sum(node: ref TreeNode): i32 {
            var sum: i32 = node.value;
            if (node.left != nil) {
                sum = sum + tree_sum(ref node.left);
            }
            if (node.right != nil) {
                sum = sum + tree_sum(ref node.right);
            }
            return sum;
        }

        fun main(): i32 {
            var left: uniq TreeNode = uniq TreeNode();
            left.value = 2;
            left.left = nil;
            left.right = nil;

            var right: uniq TreeNode = uniq TreeNode();
            right.value = 3;
            right.left = nil;
            right.right = nil;

            var root: uniq TreeNode = uniq TreeNode();
            root.value = 1;
            root.left = left;
            root.right = right;

            return tree_sum(ref root);
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE("implicit destruction") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun make_list(): i32 {
            var n3: uniq Node = uniq Node();
            n3.value = 3;
            n3.next = nil;

            var n2: uniq Node = uniq Node();
            n2.value = 2;
            n2.next = n3;

            var n1: uniq Node = uniq Node();
            n1.value = 1;
            n1.next = n2;

            return n1.value;
        }

        fun main(): i32 {
            return make_list();
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE("nil assignment cleans up subtree") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var n2: uniq Node = uniq Node();
            n2.value = 2;
            n2.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 1;
            n1.next = n2;

            n1.next = nil;

            return n1.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 1);
    }

    TEST_CASE("reassignment cleans up old subtree") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun main(): i32 {
            var old_next: uniq Node = uniq Node();
            old_next.value = 99;
            old_next.next = nil;

            var n1: uniq Node = uniq Node();
            n1.value = 1;
            n1.next = old_next;

            var new_next: uniq Node = uniq Node();
            new_next.value = 42;
            new_next.next = nil;
            n1.next = new_next;

            var r: ref Node = ref n1.next;
            return r.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 42);
    }

    TEST_CASE("direct self-reference compile error") {
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: Node;
        }

        fun main(): i32 {
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE("mutual recursion compile error") {
        const char* source = R"CODE(
        struct A {
            value: i32;
            b: B;
        }

        struct B {
            value: i32;
            a: A;
        }

        fun main(): i32 {
            return 0;
        }
    )CODE";

        BumpAllocator allocator(65536);
        BCModule* module = compile(allocator, source);
        CHECK(module == nullptr);
    }

    TEST_CASE("list traversal with while loop") {  // VM-only: C backend: recursive uniq-field destruction semantics gap
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun list_sum(node: ref Node): i32 {
            var sum: i32 = node.value;
            var current: ref Node = ref node;
            while (current.next != nil) {
                current = ref current.next;
                sum = sum + current.value;
            }
            return sum;
        }

        fun main(): i32 {
            var n5: uniq Node = uniq Node();
            n5.value = 5;
            n5.next = nil;

            var n4: uniq Node = uniq Node();
            n4.value = 4;
            n4.next = n5;

            var n3: uniq Node = uniq Node();
            n3.value = 3;
            n3.next = n4;

            var n2: uniq Node = uniq Node();
            n2.value = 2;
            n2.next = n3;

            var n1: uniq Node = uniq Node();
            n1.value = 1;
            n1.next = n2;

            return list_sum(ref n1);
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 15);
    }

    TEST_CASE("tagged union AST eval") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        const char* source = R"CODE(
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
                case Negate: return -eval(ref e.operand);
                case Add: return eval(ref e.left) + eval(ref e.right);
            }
            return 0;
        }

        fun main(): i32 {
            // (-7) + 5 = -2
            var lit5: uniq Expr = uniq Expr { kind = ExprKind::Literal, value = 5 };
            var lit7: uniq Expr = uniq Expr { kind = ExprKind::Literal, value = 7 };
            var neg: uniq Expr = uniq Expr { kind = ExprKind::Negate, operand = lit7 };
            var add: uniq Expr = uniq Expr { kind = ExprKind::Add, left = neg, right = lit5 };

            return eval(ref add);
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == -2);
    }

    TEST_CASE_TEMPLATE("mutual recursion via List<uniq T>", Backend, RX_E2E_BACKENDS) {
        const char* source = R"CODE(
        struct Forest {
            trees: List<uniq Tree>;
        }

        struct Tree {
            value: i32;
            children: Forest;
        }

        fun main(): i32 {
            var leaf_a: uniq Tree = uniq Tree {
                value = 2,
                children = Forest { trees = List<uniq Tree>() }
            };
            var leaf_b: uniq Tree = uniq Tree {
                value = 3,
                children = Forest { trees = List<uniq Tree>() }
            };

            var root: uniq Tree = uniq Tree {
                value = 1,
                children = Forest { trees = List<uniq Tree>() }
            };
            root.children.trees.push(leaf_a);
            root.children.trees.push(leaf_b);

            var sum: i32 = root.value;
            sum = sum + root.children.trees[0].value;
            sum = sum + root.children.trees[1].value;
            return sum;
        }
    )CODE";

        auto result = Backend::run(source);
        CHECK(result.success);
        CHECK(result.value == 6);
    }

    TEST_CASE("deep linked list construction and destruction") {  // VM-only: result exceeds 0..255 exit-code range (C exit code is 8-bit)
        const char* source = R"CODE(
        struct Node {
            value: i32;
            next: uniq Node;
        }

        fun build(n: i32): uniq Node {
            var head: uniq Node = uniq Node { value = 0, next = nil };
            var i: i32 = 1;
            while (i < n) {
                var new_head: uniq Node = uniq Node { value = i, next = head };
                head = new_head;
                i = i + 1;
            }
            return head;
        }

        fun main(): i32 {
            var list: uniq Node = build(500);
            return list.value;
        }
    )CODE";

        auto result = VMBackend::run(source);
        CHECK(result.success);
        CHECK(result.value == 499);
    }

}  // TEST_SUITE("E2E Recursive Types")

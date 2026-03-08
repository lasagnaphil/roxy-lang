#include "roxy/core/doctest/doctest.h"
#include "test_helpers.hpp"

using namespace rx;

// ============================================================================
// Recursive Types Tests
// ============================================================================

TEST_CASE("E2E - Recursive types: basic uniq self-reference compiles") {
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

TEST_CASE("E2E - Recursive types: two-node list") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
}

TEST_CASE("E2E - Recursive types: access uniq field via ref param") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 20);
}

TEST_CASE("E2E - Recursive types: explicit ref expression") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 10);
}

TEST_CASE("E2E - Recursive types: ref of uniq field") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 20);
}

TEST_CASE("E2E - Recursive types: linked list traversal with ref") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 60);
}

TEST_CASE("E2E - Recursive types: binary tree sum") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 6);
}

TEST_CASE("E2E - Recursive types: implicit destruction") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Recursive types: nil assignment cleans up subtree") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 1);
}

TEST_CASE("E2E - Recursive types: reassignment cleans up old subtree") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 42);
}

TEST_CASE("E2E - Recursive types: direct self-reference compile error") {
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

TEST_CASE("E2E - Recursive types: mutual recursion compile error") {
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

TEST_CASE("E2E - Recursive types: list traversal with while loop") {
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

    TestResult result = run_and_capture(source, "main");
    CHECK(result.success);
    CHECK(result.value == 15);
}

#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/core/string.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/type_env.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>
#include <cstdio>

using namespace rx;

// Helper to parse and analyze source code
struct SemanticTestHelper {
    BumpAllocator allocator{4096};
    TypeEnv* type_env = nullptr;
    ModuleRegistry* modules = nullptr;
    NativeRegistry* natives = nullptr;
    SemanticAnalyzer* analyzer = nullptr;
    Program* program = nullptr;
    bool parse_ok = false;
    bool analyze_ok = false;

    bool run(const char* source) {
        Lexer lexer(source, static_cast<u32>(strlen(source)));
        Parser parser(lexer, allocator);

        program = parser.parse();
        parse_ok = !parser.has_error();

        if (!parse_ok) {
            return false;
        }

        type_env = allocator.emplace<TypeEnv>(allocator);
        modules = allocator.emplace<ModuleRegistry>(allocator);
        natives = allocator.emplace<NativeRegistry>(allocator, type_env->types());
        register_builtin_natives(*natives);
        modules->register_native_module(BUILTIN_MODULE_NAME, natives, type_env->types());

        analyzer = allocator.emplace<SemanticAnalyzer>(allocator, *type_env, *modules);
        analyze_ok = analyzer->analyze(program);
        return analyze_ok;
    }

    bool has_error_containing(const char* substring) const {
        if (!analyzer) return false;
        for (const auto& err : analyzer->errors()) {
            if (strstr(err.message, substring)) {
                return true;
            }
        }
        return false;
    }

    u32 error_count() const {
        if (!analyzer) return 0;
        return analyzer->errors().size();
    }
};

// ============================================================================
// Type System Tests
// ============================================================================

TEST_SUITE("Semantic") {

    TEST_CASE("Types: Primitive type creation") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        CHECK(types.void_type()->kind == TypeKind::Void);
        CHECK(types.bool_type()->kind == TypeKind::Bool);
        CHECK(types.i32_type()->kind == TypeKind::I32);
        CHECK(types.i64_type()->kind == TypeKind::I64);
        CHECK(types.f32_type()->kind == TypeKind::F32);
        CHECK(types.f64_type()->kind == TypeKind::F64);
        CHECK(types.string_type()->kind == TypeKind::String);
        CHECK(types.nil_type()->kind == TypeKind::Nil);
        CHECK(types.error_type()->kind == TypeKind::Error);
    }

    TEST_CASE("Types: Primitive type lookup by name") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        CHECK(types.primitive_by_name("void") == types.void_type());
        CHECK(types.primitive_by_name("bool") == types.bool_type());
        CHECK(types.primitive_by_name("i32") == types.i32_type());
        CHECK(types.primitive_by_name("f64") == types.f64_type());
        CHECK(types.primitive_by_name("string") == types.string_type());
        CHECK(types.primitive_by_name("unknown") == nullptr);
    }

    TEST_CASE("Types: List type interning") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        Type* lst1 = types.list_type(types.i32_type());
        Type* lst2 = types.list_type(types.i32_type());
        Type* lst3 = types.list_type(types.f64_type());

        CHECK(lst1 == lst2);  // Same element type -> same interned type
        CHECK(lst1 != lst3);  // Different element type -> different type
        CHECK(lst1->kind == TypeKind::List);
        CHECK(lst1->list_info.element_type == types.i32_type());
    }

    TEST_CASE("Types: Reference type interning") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        Type* uniq1 = types.uniq_type(types.i32_type());
        Type* uniq2 = types.uniq_type(types.i32_type());
        Type* ref1 = types.ref_type(types.i32_type());

        CHECK(uniq1 == uniq2);  // Same kind and inner -> interned
        CHECK(uniq1 != ref1);   // Different kind
        CHECK(uniq1->kind == TypeKind::Uniq);
        CHECK(ref1->kind == TypeKind::Ref);
    }

    TEST_CASE("Types: Type helper methods") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        CHECK(types.i32_type()->is_integer());
        CHECK(types.i32_type()->is_signed_integer());
        CHECK(types.u64_type()->is_unsigned_integer());
        CHECK(types.f32_type()->is_float());
        CHECK(types.f64_type()->is_numeric());
        CHECK(types.i32_type()->is_numeric());
        CHECK(!types.bool_type()->is_numeric());

        Type* uniq = types.uniq_type(types.i32_type());
        CHECK(uniq->is_reference());
        CHECK(uniq->inner_type() == types.i32_type());
        CHECK(uniq->base_type() == types.i32_type());
    }

    // ============================================================================
    // Symbol Table Tests
    // ============================================================================

    TEST_CASE("SymbolTable: Basic scope management") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);
        SymbolTable symbols(allocator);

        SourceLocation loc = {0, 1, 1};

        // Define in global scope
        Symbol* x = symbols.define(SymbolKind::Variable, "x", types.i32_type(), loc);
        CHECK(x != nullptr);
        CHECK(symbols.lookup("x") == x);

        // Push a new scope
        symbols.push_scope(ScopeKind::Block);

        // Still visible
        CHECK(symbols.lookup("x") == x);

        // Define in inner scope (shadows outer)
        Symbol* x2 = symbols.define(SymbolKind::Variable, "x", types.f64_type(), loc);
        CHECK(symbols.lookup("x") == x2);
        CHECK(x2->type == types.f64_type());

        // Pop scope, original visible again
        symbols.pop_scope();
        CHECK(symbols.lookup("x") == x);
        CHECK(symbols.lookup("x")->type == types.i32_type());
    }

    TEST_CASE("SymbolTable: incremental lookup-cache maintenance") {
        // pop_scope restores each name's displaced entry instead of rebuilding
        // the cache from the whole scope chain; lookup_local answers from the
        // cache via a defining_scope check. Pin the invariants that makes safe.
        BumpAllocator allocator{1024};
        TypeCache types(allocator);
        SymbolTable symbols(allocator);
        SourceLocation loc = {0, 1, 1};

        SUBCASE("same-name redefinition within one scope unwinds correctly") {
            // The flat enum-variant namespace defines the same name twice in
            // one scope (later shadows earlier). In an inner scope, a pop
            // must unwind both entries back to the outer symbol.
            Symbol* outer = symbols.define(SymbolKind::Variable, "v", types.i32_type(), loc);

            symbols.push_scope(ScopeKind::Block);
            Symbol* inner1 = symbols.define(SymbolKind::Variable, "v", types.f32_type(), loc);
            Symbol* inner2 = symbols.define(SymbolKind::Variable, "v", types.f64_type(), loc);
            CHECK(symbols.lookup("v") == inner2);
            CHECK(inner1->shadowed == outer);
            CHECK(inner2->shadowed == inner1);

            symbols.pop_scope();
            CHECK(symbols.lookup("v") == outer);
        }

        SUBCASE("multi-level shadowing restores per level") {
            Symbol* g = symbols.define(SymbolKind::Variable, "x", types.i32_type(), loc);
            symbols.push_scope(ScopeKind::Block);
            Symbol* mid = symbols.define(SymbolKind::Variable, "x", types.f32_type(), loc);
            symbols.push_scope(ScopeKind::Block);
            Symbol* deep = symbols.define(SymbolKind::Variable, "x", types.f64_type(), loc);

            CHECK(symbols.lookup("x") == deep);
            symbols.pop_scope();
            CHECK(symbols.lookup("x") == mid);
            symbols.pop_scope();
            CHECK(symbols.lookup("x") == g);
        }

        SUBCASE("lookup_local sees only the current scope") {
            symbols.define(SymbolKind::Variable, "y", types.i32_type(), loc);
            symbols.push_scope(ScopeKind::Block);

            // Visible but not local.
            CHECK(symbols.lookup("y") != nullptr);
            CHECK(symbols.lookup_local("y") == nullptr);

            // Local after a same-scope definition.
            Symbol* local = symbols.define(SymbolKind::Variable, "y", types.f64_type(), loc);
            CHECK(symbols.lookup_local("y") == local);

            // Sibling scope: previous block's local is neither local nor stale.
            symbols.pop_scope();
            symbols.push_scope(ScopeKind::Block);
            CHECK(symbols.lookup_local("y") == nullptr);
            CHECK(symbols.lookup("y")->type == types.i32_type());
            symbols.pop_scope();
        }

        SUBCASE("popping a scope with an unshadowed name erases it") {
            symbols.push_scope(ScopeKind::Block);
            symbols.define(SymbolKind::Variable, "only_inner", types.i32_type(), loc);
            CHECK(symbols.lookup("only_inner") != nullptr);
            symbols.pop_scope();
            CHECK(symbols.lookup("only_inner") == nullptr);
            CHECK(symbols.lookup_local("only_inner") == nullptr);
        }
    }

    TEST_CASE("SymbolTable: Function scope") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);
        SymbolTable symbols(allocator);

        CHECK(!symbols.is_in_function());

        symbols.push_function_scope(types.i32_type());
        CHECK(symbols.is_in_function());
        CHECK(symbols.current_return_type() == types.i32_type());

        symbols.pop_scope();
        CHECK(!symbols.is_in_function());
    }

    TEST_CASE("SymbolTable: Loop scope") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);
        SymbolTable symbols(allocator);

        CHECK(!symbols.is_in_loop());

        symbols.push_loop_scope();
        CHECK(symbols.is_in_loop());

        symbols.push_scope(ScopeKind::Block);
        CHECK(symbols.is_in_loop());  // Still in loop, nested block

        symbols.pop_scope();
        symbols.pop_scope();
        CHECK(!symbols.is_in_loop());
    }

    // ============================================================================
    // Semantic Analysis Tests - Valid Code
    // ============================================================================

    TEST_CASE("Semantic: Variable declaration with type") {
        SemanticTestHelper t;
        CHECK(t.run("var x: i32 = 42;"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Variable declaration with type inference") {
        SemanticTestHelper t;
        CHECK(t.run("var x = 42;"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Function declaration and call") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }

        fun main() {
            var result = add(1, 2);
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Struct declaration") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct Point {
            x: f32;
            y: f32;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Enum declaration") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        enum Color {
            Red,
            Green,
            Blue
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Control flow statements") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test(x: i32): i32 {
            if (x > 0) {
                return 1;
            } else {
                return 0;
            }
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: While loop") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun countdown(n: i32) {
            var i = n;
            while (i > 0) {
                i = i - 1;
            }
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: For loop") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun sum(n: i32): i32 {
            var result = 0;
            for (var i = 0; i < n; i = i + 1) {
                result = result + i;
            }
            return result;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Break and continue in loops") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test() {
            while (true) {
                break;
            }
            for (var i = 0; i < 10; i = i + 1) {
                if (i == 5) {
                    continue;
                }
            }
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: List indexing") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test(lst: List<i32>): i32 {
            return lst[0];
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Binary operators") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test() {
            var a = 1 + 2;
            var b = 3.14 * 2.0;
            var c = true && false;
            var d = 1 < 2;
            var e = 0xFF & 0x0F;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Unary operators") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test() {
            var a = -42;
            var b = !true;
            var c = ~0xFF;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Ternary expression") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun max(a: i32, b: i32): i32 {
            return a > b ? a : b;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    // ============================================================================
    // Semantic Analysis Tests - Error Detection
    // ============================================================================

    TEST_CASE("Semantic Error: Undefined identifier") {
        SemanticTestHelper t;
        CHECK(!t.run("var x = undefined_var;"));
        CHECK(t.has_error_containing("undefined"));
    }

    TEST_CASE("Semantic Error: Type mismatch in assignment") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = "hello";
        }
    )"));
        CHECK(t.has_error_containing("cannot assign"));
    }

    TEST_CASE("Semantic Error: Non-boolean condition") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            if (42) {}
        }
    )"));
        CHECK(t.has_error_containing("boolean"));
    }

    TEST_CASE("Semantic Error: Break outside loop") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            break;
        }
    )"));
        CHECK(t.has_error_containing("outside of loop"));
    }

    TEST_CASE("Semantic Error: Continue outside loop") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            continue;
        }
    )"));
        CHECK(t.has_error_containing("outside of loop"));
    }

    TEST_CASE("Semantic Error: Return type mismatch") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test(): i32 {
            return "hello";
        }
    )"));
        CHECK(t.has_error_containing("cannot assign"));
    }

    TEST_CASE("Semantic Error: Non-void function without return value") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test(): i32 {
            return;
        }
    )"));
        CHECK(t.has_error_containing("must return a value"));
    }

    TEST_CASE("Semantic Error: Wrong argument count") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun add(a: i32, b: i32): i32 {
            return a + b;
        }

        fun test() {
            var x = add(1);
        }
    )"));
        CHECK(t.has_error_containing("arguments"));
    }

    TEST_CASE("Semantic Error: Duplicate type declaration") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Foo {}
        struct Foo {}
    )"));
        CHECK(t.has_error_containing("duplicate"));
    }

    TEST_CASE("Semantic Error: Duplicate variable in same scope") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x = 1;
            var x = 2;
        }
    )"));
        CHECK(t.has_error_containing("redefinition"));
    }

    // ── Local-shadowing ban (C#/Java tier) ─────────────────────────────────
    // A local declaration (`var`, catch variable, lambda parameter) may not
    // reuse a name bound to a variable or parameter of the current function,
    // including across lambda boundaries. Module-level names (globals,
    // functions) stay shadowable, and sequential (non-overlapping) scopes may
    // reuse names. Before the ban, shadowing programs passed semantic analysis
    // and were miscompiled by the name-keyed IR builder (wrong-value rebinds,
    // leaked uniq locals).

    TEST_CASE("Semantic Error: local shadows local in nested block") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 1;
            {
                var x: i32 = 2;
            }
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("Semantic Error: local shadows parameter") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test(n: i32) {
            {
                var n: i32 = 2;
            }
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
        CHECK(t.has_error_containing("parameter"));
    }

    TEST_CASE("Semantic Error: for initializer shadows local") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var i: i32 = 0;
            for (var i: i32 = 0; i < 10; i = i + 1) {
            }
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("Semantic Error: catch variable shadows local") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var e: i32 = 1;
            try {
                var y: i32 = 2;
            } catch (e) {
            }
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("Semantic Error: lambda parameter shadows enclosing local") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 1;
            var f = fun(x: i32): i32 => x + 1;
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("Semantic Error: lambda body local shadows enclosing local") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 1;
            var f = fun(): i32 {
                var x: i32 = 2;
                return x;
            };
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("Semantic Error: nested lambda shadows across two boundaries") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 1;
            var f = fun(): i32 {
                var g = fun(): i32 {
                    var x: i32 = 2;
                    return x;
                };
                return g();
            };
        }
    )"));
        CHECK(t.has_error_containing("shadows"));
    }

    TEST_CASE("local shadowing a global is allowed") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        var g: i32 = 1;
        fun test(): i32 {
            var g: i32 = 2;
            return g;
        }
    )"));
    }

    TEST_CASE("local named after a function is allowed") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun helper(): i32 { return 1; }
        fun test(): i32 {
            var helper: i32 = 2;
            return helper;
        }
    )"));
    }

    TEST_CASE("sequential scopes may reuse a name") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun test() {
            {
                var t: i32 = 1;
            }
            {
                var t: i32 = 2;
            }
            for (var i: i32 = 0; i < 3; i = i + 1) {}
            for (var i: i32 = 0; i < 3; i = i + 1) {}
        }
    )"));
    }

    TEST_CASE("Semantic Error: Unknown type") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        var x: UnknownType = nil;
    )"));
        CHECK(t.has_error_containing("unknown type"));
    }

    TEST_CASE("Semantic Error: Unknown parent type") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Child : UnknownParent {}
    )"));
        CHECK(t.has_error_containing("unknown parent"));
    }

    // A `ref` field is now allowed: the struct becomes move-only and counts the
    // borrow (lifetimes.md §18). A self-referential `ref` is fine — it
    // borrows another node, it doesn't own one (no ownership cycle).
    TEST_CASE("ref field is accepted (move-only counted borrow)") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct Node {
            next: ref Node;
        }
    )"));
    }

    TEST_CASE("Semantic Error: Delete on non-uniq") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 42;
            delete x;
        }
    )"));
        CHECK(t.has_error_containing("uniq"));
    }

    TEST_CASE("Semantic Error: Indexing non-list") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 42;
            var y = x[0];
        }
    )"));
        CHECK(t.has_error_containing("index"));
    }

    TEST_CASE("Semantic Error: Member access on non-struct") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 42;
            var y = x.field;
        }
    )"));
        CHECK(t.has_error_containing("non-struct"));
    }

    TEST_CASE("Semantic Error: Unknown struct field") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Point { x: f32; y: f32; }
        fun test(p: Point) {
            var z = p.z;
        }
    )"));
        CHECK(t.has_error_containing("no member"));
    }

    TEST_CASE("Semantic Error: Calling non-callable") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 42;
            x();
        }
    )"));
        CHECK(t.has_error_containing("not callable"));
    }

    TEST_CASE("Semantic Error: Arithmetic on non-numeric") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x = true + false;
        }
    )"));
        CHECK(t.has_error_containing("invalid operands"));
    }

    TEST_CASE("Semantic Error: Assignment to non-lvalue") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            1 + 2 = 3;
        }
    )"));
        CHECK(t.has_error_containing("lvalue"));
    }

    TEST_CASE("Semantic Error: Variable without type or initializer") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        fun test() {
            var x;
        }
    )"));
        CHECK(t.error_count() > 0);
    }

    // ============================================================================
    // Reference Type Tests
    // ============================================================================

    TEST_CASE("Semantic: Uniq type creation with uniq") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct Point { x: f32; y: f32; }
        fun test() {
            var p: uniq Point = uniq Point();
            delete p;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Stack type creation") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct Point { x: f32; y: f32; }
        fun test() {
            var p: Point = Point();
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: Constructor call with args but no constructor") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Point { x: f32; y: f32; }
        fun test() {
            var p: uniq Point = uniq Point(1.0, 2.0);
        }
    )"));
        CHECK(t.error_count() == 1);
        CHECK(t.has_error_containing("has no constructor"));
    }

    TEST_CASE("Semantic: Struct inheritance") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct Animal {
            name: string;
        }
        struct Dog : Animal {
            breed: string;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    // ============================================================================
    // Type String Tests
    // ============================================================================

    TEST_CASE("Types: type_to_string") {
        BumpAllocator allocator{1024};
        TypeCache types(allocator);

        SUBCASE("Primitive types") {
            String out;
            type_to_string(types.i32_type(), out);
            out.push_back('\0');
            CHECK(strcmp(out.data(), "i32") == 0);
        }

        SUBCASE("List types") {
            String out;
            Type* lst = types.list_type(types.f64_type());
            type_to_string(lst, out);
            out.push_back('\0');
            CHECK(strcmp(out.data(), "List<f64>") == 0);
        }

        SUBCASE("Reference types") {
            String out;
            Type* uniq = types.uniq_type(types.i32_type());
            type_to_string(uniq, out);
            out.push_back('\0');
            CHECK(strcmp(out.data(), "uniq i32") == 0);
        }
    }

    // ============================================================================
    // Deep-review regression tests (see TODO.md "Semantic Analyzer Refactoring
    // & Bugs" — one block per verified bug)
    // ============================================================================

    TEST_CASE("Semantic: missing trait method error renders names (not %.*s)") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        trait Greet;
        fun Greet.hello(): string;
        fun Greet.bye(): string;

        struct P { x: i32 = 0; }

        fun P.hello(): string for Greet {
            return "hi";
        }
    )"));
        CHECK(t.has_error_containing("trait 'Greet' requires method 'bye' which is not implemented for 'P'"));
        CHECK(!t.has_error_containing("%.*s"));
    }

    TEST_CASE("Semantic: when clause rejects variants of a different enum") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        enum Color { Red, Green }
        enum Size { Small, Big }

        struct Item {
            when c: Color {
                case Small:
                    n: i32;
                case Green:
                    m: i32;
            }
        }
    )"));
        CHECK(t.has_error_containing("'Small' is not a variant of enum 'Color'"));
        // Green IS a Color variant — it must not be flagged.
        CHECK(!t.has_error_containing("'Green'"));
    }

    TEST_CASE("Semantic: Coro<T> unifies during generic inference") {
        // Coroutine values are first-class: inference binds T from the yield
        // type, and the per-function coroutine value is assignable to the
        // annotated Coro<T> parameter (interned generic type) — dispatch is
        // dynamic, so the two representations unify.
        SemanticTestHelper t;
        CHECK(t.run(R"(
        fun make(): Coro<i32> {
            yield 1;
        }

        fun first<T>(c: Coro<T>): i32 {
            return 0;
        }

        fun test(): i32 {
            var c = make();
            return first(c);
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: coroutine methods are supported (self is a captured ref param)") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        struct S { n: i32 = 3; }

        fun S.count(): Coro<i32> {
            var i: i32 = self.n;
            while (i > 0) {
                yield i;
                i = i - 1;
            }
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: coroutine methods on generic structs are rejected clearly") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Box<T> { value: T; }

        fun Box<T>.gen(): Coro<T> {
            yield self.value;
        }

        fun main(): i32 {
            var b: Box<i32> = Box { value = 5 };
            return 0;
        }
    )"));
        CHECK(t.has_error_containing("not yet supported on generic structs or in traits"));
        // Not the misleading yield-placement error.
        CHECK(!t.has_error_containing("'yield' can only appear inside a coroutine function"));
    }

    TEST_CASE("Semantic: coroutine trait-impl methods are rejected clearly") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        trait Gen;
        fun Gen.produce(): Coro<i32>;

        struct S { n: i32 = 1; }

        fun S.produce(): Coro<i32> for Gen {
            yield self.n;
        }
    )"));
        CHECK(t.has_error_containing("not yet supported on generic structs or in traits"));
        CHECK(!t.has_error_containing("'yield' can only appear inside a coroutine function"));
    }

    TEST_CASE("Semantic: same-named variants of different enums don't collide") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        enum A { X, Y }
        enum B { X, Z }

        fun test(): bool {
            var v: A = A::X;
            var w: B = B::X;
            return v == A::X;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: tagged-union variant field can reference a later struct") {
        SemanticTestHelper t;
        CHECK(t.run(R"(
        enum Kind { Solo, Boxed }

        struct Holder {
            when k: Kind {
                case Solo:
                    n: i32;
                case Boxed:
                    item: Item;
            }
        }

        struct Item {
            a: i32;
            b: i32;
        }
    )"));
        CHECK(t.error_count() == 0);
    }

    TEST_CASE("Semantic: three-struct value cycle still reports infinite size") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct A { b: B; }
        struct B { c: C; }
        struct C { a: A; }
    )"));
        CHECK(t.has_error_containing("has infinite size"));
    }

    TEST_CASE("Semantic: unsupported primitive operators error loudly") {
        // Float modulo is now the only "numeric type, same type, no registered
        // operator" case left: narrow ints promote to i32, and u32/u64 have native
        // arithmetic. (string/bool arithmetic reports "invalid operands" instead.)
        SUBCASE("float modulo") {
            SemanticTestHelper t;
            CHECK(!t.run(R"(
            fun test(): f32 {
                var x: f32 = 5.0f;
                var y: f32 = 2.0f;
                return x % y;
            }
        )"));
            CHECK(t.has_error_containing("operator '%' is not supported for type 'f32'"));
        }

        SUBCASE("u32 arithmetic is now supported") {
            SemanticTestHelper t;
            CHECK(t.run(R"(
            fun test() {
                var x: u32 = 40;
                var y: u32 = 2;
                var z = x + y;
                var less = x < y;
                var neg = -x;
            }
        )"));
            CHECK(t.error_count() == 0);
        }

        SUBCASE("u64 equality is supported") {
            SemanticTestHelper t;
            CHECK(t.run(R"(
            fun test(): bool {
                var x: u64 = 5ul;
                var y: u64 = 5ul;
                return x == y;
            }
        )"));
            CHECK(t.error_count() == 0);
        }
    }

    TEST_CASE("Semantic: Printable error renders the type name (not %s)") {
        SemanticTestHelper t;
        CHECK(!t.run(R"(
        struct Point { x: f32 = 0.0f; y: f32 = 0.0f; }
        fun test() {
            var p: Point = Point { x = 1.0f, y = 2.0f };
            print(f"{p}");
        }
    )"));
        CHECK(t.has_error_containing("type 'Point' does not implement Printable"));
        CHECK(!t.has_error_containing("%s'"));
    }

    // ========================================================================
    // Error recovery: the analyzer accumulates many genuine errors in one pass
    // and substitutes never-null `error_type` sentinels so a local failure does
    // not cascade. These pin the exact recovery behavior (see
    // docs/internals/error-handling.md); the many single-error cases above
    // check individual diagnostics, these check the recovery machinery itself.
    // ========================================================================

    TEST_CASE("Semantic recovery: independent errors accumulate across functions") {
        // Recovery continues across declarations: each of three functions
        // carries one independent error, and all three surface in a single pass
        // (a fail-fast analyzer would stop at the first).
        SemanticTestHelper t;
        t.run(R"(
            fun a(): i32 { return true; }
            fun b() { undefined_var; }
            fun c(x: i32): i32 { if (x > 0) { return 1; } }
        )");
        CHECK(t.error_count() == 3);
        CHECK(t.has_error_containing("cannot assign 'bool' to 'i32'"));
        CHECK(t.has_error_containing("undefined identifier 'undefined_var'"));
        CHECK(t.has_error_containing("not all code paths return a value"));
    }

    TEST_CASE("Semantic recovery: undefined identifier does not cascade") {
        // The undefined name resolves to error_type once; every later use of the
        // resulting value (arithmetic, f-string) is inert — one diagnostic total.
        SemanticTestHelper t;
        t.run(R"(
            fun f() {
                var x = undefined_thing;
                var a = x + 1;
                var b = x + 2;
                var c = x * 3;
                print(f"{x}");
            }
        )");
        CHECK(t.error_count() == 1);
        CHECK(t.has_error_containing("undefined identifier 'undefined_thing'"));
    }

    TEST_CASE("Semantic recovery: unknown type annotation does not cascade") {
        // A bad annotation yields error_type for x; arithmetic and comparisons
        // on an error_type operand short-circuit, so only the unknown-type error
        // is reported.
        SemanticTestHelper t;
        t.run(R"(
            fun f() {
                var x: NotAType = 5;
                var y = x + 1;
                var z = x * 2;
                if (x > 0) { }
            }
        )");
        CHECK(t.error_count() == 1);
        CHECK(t.has_error_containing("unknown type 'NotAType'"));
    }

    TEST_CASE("Semantic recovery: wrong argument count does not cascade") {
        // The call error is reported once; using its result in further
        // arithmetic adds nothing.
        SemanticTestHelper t;
        t.run(R"(
            fun g(a: i32, b: i32): i32 { return a + b; }
            fun f() {
                var r = g(1);
                var s = r + 1;
            }
        )");
        CHECK(t.error_count() == 1);
        CHECK(t.has_error_containing("expected 2 arguments but got 1"));
    }

    TEST_CASE("Semantic recovery: repeated bad field access reports per occurrence") {
        // Two distinct `p.bogus` sites are two genuine errors (reported once
        // each), but the `+ 1` on the error-typed result of the second does NOT
        // add a third — pins both per-occurrence reporting and the no-cascade
        // bound so a future change that collapses to 1 or leaks to 3 is caught.
        SemanticTestHelper t;
        t.run(R"(
            struct P { x: i32; }
            fun f() {
                var p: P;
                var a = p.bogus;
                var b = p.bogus + 1;
            }
        )");
        CHECK(t.error_count() == 2);
        CHECK(t.has_error_containing("struct 'P' has no member 'bogus'"));
    }

    TEST_CASE("Semantic recovery: error collection is capped at MAX_SEMANTIC_ERRORS") {
        // A pathological body with far more than the cap of independent errors
        // must not spew unboundedly — batch-mode collection stops at the cap.
        rx::String src;
        src.append("fun f() {\n");
        char line[64];
        for (int i = 0; i < 40; i++) {
            snprintf(line, sizeof(line), "  undefined_%d;\n", i);
            src.append(line);
        }
        src.append("}\n");

        SemanticTestHelper t;
        t.run(src.c_str());
        CHECK(t.error_count() == MAX_SEMANTIC_ERRORS);  // 20 in batch mode
    }

}  // TEST_SUITE("Semantic")

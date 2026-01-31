#include "roxy/core/doctest/doctest.h"

#include "roxy/core/bump_allocator.hpp"
#include "roxy/shared/lexer.hpp"
#include "roxy/compiler/parser.hpp"
#include "roxy/compiler/semantic.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/natives.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cstring>

using namespace rx;

// Helper to parse and analyze source code
struct SemanticTestHelper {
    BumpAllocator allocator{4096};
    TypeCache* types = nullptr;
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

        types = allocator.emplace<TypeCache>(allocator);
        modules = allocator.emplace<ModuleRegistry>(allocator);
        natives = allocator.emplace<NativeRegistry>(allocator, *types);
        register_builtin_natives(*natives);
        modules->register_native_module(BUILTIN_MODULE_NAME, natives, *types);

        analyzer = allocator.emplace<SemanticAnalyzer>(allocator, *types, *modules);
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

TEST_CASE("Types: Array type interning") {
    BumpAllocator allocator{1024};
    TypeCache types(allocator);

    Type* arr1 = types.array_type(types.i32_type());
    Type* arr2 = types.array_type(types.i32_type());
    Type* arr3 = types.array_type(types.f64_type());

    CHECK(arr1 == arr2);  // Same element type -> same interned type
    CHECK(arr1 != arr3);  // Different element type -> different type
    CHECK(arr1->kind == TypeKind::Array);
    CHECK(arr1->array_info.element_type == types.i32_type());
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

TEST_CASE("Semantic: Array indexing") {
    SemanticTestHelper t;
    CHECK(t.run(R"(
        fun test(arr: i32[]): i32 {
            return arr[0];
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

TEST_CASE("Semantic Error: Ref in struct field") {
    SemanticTestHelper t;
    CHECK(!t.run(R"(
        struct Node {
            next: ref Node;
        }
    )"));
    CHECK(t.has_error_containing("ref"));
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

TEST_CASE("Semantic Error: Indexing non-array") {
    SemanticTestHelper t;
    CHECK(!t.run(R"(
        fun test() {
            var x: i32 = 42;
            var y = x[0];
        }
    )"));
    CHECK(t.has_error_containing("non-array"));
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
        Vector<char> out;
        type_to_string(types.i32_type(), out);
        out.push_back('\0');
        CHECK(strcmp(out.data(), "i32") == 0);
    }

    SUBCASE("Array types") {
        Vector<char> out;
        Type* arr = types.array_type(types.f64_type());
        type_to_string(arr, out);
        out.push_back('\0');
        CHECK(strcmp(out.data(), "f64[]") == 0);
    }

    SUBCASE("Reference types") {
        Vector<char> out;
        Type* uniq = types.uniq_type(types.i32_type());
        type_to_string(uniq, out);
        out.push_back('\0');
        CHECK(strcmp(out.data(), "uniq i32") == 0);
    }
}

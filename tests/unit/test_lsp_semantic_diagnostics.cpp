#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/lsp_type_resolver.hpp"
#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/lsp/global_index.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to set up a GlobalIndex with given source
static void setup_index(GlobalIndex& index, const char* source, BumpAllocator& allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    FileIndexer indexer;
    FileStubs stubs = indexer.index(tree.root);
    String uri("file:///test.roxy");
    index.update_file(uri, stubs);
}

// Helper to parse, lower a function, and analyze with diagnostics
static const Vector<SemanticDiagnostic>& analyze_with_diagnostics(
    const char* source, LspTypeResolver& resolver,
    BumpAllocator& parse_allocator, BumpAllocator& ast_allocator) {

    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, parse_allocator);
    SyntaxTree tree = parser.parse();

    // Find the first function/method/constructor/destructor declaration
    SyntaxNode* fn_node = nullptr;
    for (u32 i = 0; i < tree.root->children.size(); i++) {
        SyntaxKind kind = tree.root->children[i]->kind;
        if (kind == SyntaxKind::NodeFunDecl || kind == SyntaxKind::NodeMethodDecl ||
            kind == SyntaxKind::NodeConstructorDecl || kind == SyntaxKind::NodeDestructorDecl) {
            fn_node = tree.root->children[i];
            break;
        }
    }

    if (fn_node) {
        CstLowering lowering(ast_allocator);
        Decl* ast_decl = lowering.lower_decl(fn_node);
        resolver.analyze_function_with_diagnostics(ast_decl);
    }

    return resolver.diagnostics();
}

// Helper for concise test setup
struct DiagTestContext {
    BumpAllocator index_alloc{4096};
    BumpAllocator parse_alloc{8192};
    BumpAllocator ast_alloc{4096};
    GlobalIndex index;

    void add_types(const char* type_source) {
        setup_index(index, type_source, index_alloc);
    }

    const Vector<SemanticDiagnostic>& analyze(const char* source) {
        LspTypeResolver* resolver = new LspTypeResolver(index);
        m_resolver = resolver;
        return analyze_with_diagnostics(source, *resolver, parse_alloc, ast_alloc);
    }

    ~DiagTestContext() { delete m_resolver; }
private:
    LspTypeResolver* m_resolver = nullptr;
};

// --- Test cases ---

TEST_CASE("SemanticDiagnostic: no errors in clean function body") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }\nfun add(a: i32, b: i32): i32 { return a + b; }");

    const auto& diags = ctx.analyze("fun main() { var x: i32; var p: Point; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: unresolved identifier") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { foo; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Unresolved identifier 'foo'"));
    CHECK(diags[0].severity == DiagnosticSeverity::Error);
}

TEST_CASE("SemanticDiagnostic: unknown type annotation") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: Blah; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Unknown type 'Blah'"));
}

TEST_CASE("SemanticDiagnostic: unresolved field") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { var p: Point; p.z; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("No field 'z' on type 'Point'"));
}

TEST_CASE("SemanticDiagnostic: unresolved method") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }\nfun Point.length(): f32 { return 0.0; }");

    const auto& diags = ctx.analyze("fun main() { var p: Point; p.fly(); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("No method 'fly' on type 'Point'"));
}

TEST_CASE("SemanticDiagnostic: unresolved function") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { bogus(); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Unresolved function 'bogus'"));
}

TEST_CASE("SemanticDiagnostic: unresolved enum variant") {
    DiagTestContext ctx;
    ctx.add_types("enum Color { Red, Green, Blue }");

    const auto& diags = ctx.analyze("fun main() { Color::Purple; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("No variant 'Purple' on enum 'Color'"));
}

TEST_CASE("SemanticDiagnostic: wrong argument count for function") {
    DiagTestContext ctx;
    ctx.add_types("fun add(a: i32, b: i32): i32 { return a + b; }");

    const auto& diags = ctx.analyze("fun main() { add(1); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Expected 2 arguments, got 1"));
}

TEST_CASE("SemanticDiagnostic: wrong argument count for method") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }\nfun Point.length(): f32 { return 0.0; }");

    const auto& diags = ctx.analyze("fun main() { var p: Point; p.length(1); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Expected 0 arguments, got 1"));
}

TEST_CASE("SemanticDiagnostic: cascade prevention - unknown type does not cascade to field access") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: Blah; x.foo; }");
    // Should only get "Unknown type 'Blah'", NOT also "No field 'foo' on type ''"
    // because x has unknown (empty) type, field checks are skipped
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Unknown type 'Blah'"));
}

TEST_CASE("SemanticDiagnostic: parameter is known") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun test(a: i32) { a; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: self is known in method") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun Point.get_x(): f32 { return self.x; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: struct literal with bad field") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { var p = Point { z = 1 }; }");
    REQUIRE(diags.size() == 3);
    CHECK(diags[0].message == String("No field 'z' on type 'Point'"));
    // Also reports missing required fields x and y
    CHECK(diags[1].message == String("Missing required field 'x' in struct literal 'Point'"));
    CHECK(diags[2].message == String("Missing required field 'y' in struct literal 'Point'"));
}

TEST_CASE("SemanticDiagnostic: primitives do not trigger unknown type") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var a: i32; var b: f64; var c: bool; var d: string; }");
    CHECK(diags.size() == 0);
}

// --- Phase 6: Literal type resolution + type mismatch ---

TEST_CASE("SemanticDiagnostic: var type mismatch - string assigned to i32") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: i32 = \"hello\"; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Type mismatch: expected 'i32', got 'string'"));
}

TEST_CASE("SemanticDiagnostic: var type mismatch - no error for matching types") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: i32 = 42; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: var type mismatch - bool assigned to string") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: string = true; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Type mismatch: expected 'string', got 'bool'"));
}

// --- Phase 6: Duplicate parameters ---

TEST_CASE("SemanticDiagnostic: duplicate parameter name") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun add(x: i32, x: i32): i32 { return x + x; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Duplicate parameter name 'x'"));
}

TEST_CASE("SemanticDiagnostic: no duplicate parameter - distinct names") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun add(a: i32, b: i32): i32 { return a + b; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: duplicate parameter in method") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun Point.set(v: f32, v: f32) { }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Duplicate parameter name 'v'"));
}

// --- Phase 6: Missing required fields ---

TEST_CASE("SemanticDiagnostic: missing required field") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { var p = Point { x = 1.0 }; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Missing required field 'y' in struct literal 'Point'"));
}

TEST_CASE("SemanticDiagnostic: no error when all fields provided") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { var p = Point { x = 1.0, y = 2.0 }; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: field with default not required") {
    DiagTestContext ctx;
    ctx.add_types("struct Config { name: string; debug: bool = false; }");

    const auto& diags = ctx.analyze("fun main() { var c = Config { name = \"test\" }; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: multiple missing required fields") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { var p = Point { }; }");
    REQUIRE(diags.size() == 2);
    CHECK(diags[0].message == String("Missing required field 'x' in struct literal 'Point'"));
    CHECK(diags[1].message == String("Missing required field 'y' in struct literal 'Point'"));
}

// --- Phase 6: Return type mismatch ---

TEST_CASE("SemanticDiagnostic: return type mismatch") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun f(): i32 { return \"hello\"; }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Type mismatch: expected 'i32', got 'string'"));
}

TEST_CASE("SemanticDiagnostic: return type no error for matching type") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun f(): i32 { return 42; }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: no type mismatch when initializer is unresolved") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: i32 = foo; }");
    // Should only get "Unresolved identifier 'foo'", not also a type mismatch
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Unresolved identifier 'foo'"));
}

TEST_CASE("SemanticDiagnostic: nil does not trigger type mismatch") {
    DiagTestContext ctx;

    const auto& diags = ctx.analyze("fun main() { var x: i32 = nil; }");
    CHECK(diags.size() == 0);
}

// --- Phase 6: Named constructor ---

TEST_CASE("SemanticDiagnostic: unresolved named constructor") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }");

    const auto& diags = ctx.analyze("fun main() { Point.from_polar(1.0, 2.0); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("No constructor 'from_polar' on struct 'Point'"));
}

TEST_CASE("SemanticDiagnostic: named constructor exists - no error") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }\nfun new Point.from_polar(r: f32, theta: f32) { }");

    const auto& diags = ctx.analyze("fun main() { Point.from_polar(1.0, 2.0); }");
    CHECK(diags.size() == 0);
}

TEST_CASE("SemanticDiagnostic: named constructor wrong arg count") {
    DiagTestContext ctx;
    ctx.add_types("struct Point { x: f32; y: f32; }\nfun new Point.from_polar(r: f32, theta: f32) { }");

    const auto& diags = ctx.analyze("fun main() { Point.from_polar(1.0); }");
    REQUIRE(diags.size() == 1);
    CHECK(diags[0].message == String("Expected 2 arguments, got 1"));
}

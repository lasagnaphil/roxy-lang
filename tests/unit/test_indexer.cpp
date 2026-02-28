#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/lsp/indexer.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse and index source
static FileStubs index_source(const char* source, BumpAllocator& allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, allocator);
    SyntaxTree tree = parser.parse();

    FileIndexer indexer;
    return indexer.index(tree.root);
}

TEST_CASE("Indexer: Empty source") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("", allocator);

    CHECK(stubs.structs.empty());
    CHECK(stubs.enums.empty());
    CHECK(stubs.functions.empty());
    CHECK(stubs.methods.empty());
    CHECK(stubs.constructors.empty());
    CHECK(stubs.destructors.empty());
    CHECK(stubs.traits.empty());
    CHECK(stubs.imports.empty());
    CHECK(stubs.globals.empty());
}

TEST_CASE("Indexer: Variable declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("var x: i32 = 42;", allocator);

    REQUIRE(stubs.globals.size() == 1);
    CHECK(stubs.globals[0].name == StringView("x"));
    CHECK(stubs.globals[0].type.name == StringView("i32"));
    CHECK(stubs.globals[0].has_initializer == true);
    CHECK(stubs.globals[0].range.start == 0);
}

TEST_CASE("Indexer: Variable without initializer") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("var y: f64;", allocator);

    REQUIRE(stubs.globals.size() == 1);
    CHECK(stubs.globals[0].name == StringView("y"));
    CHECK(stubs.globals[0].type.name == StringView("f64"));
    CHECK(stubs.globals[0].has_initializer == false);
}

TEST_CASE("Indexer: Function declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun add(a: i32, b: i32): i32 { return a + b; }", allocator);

    REQUIRE(stubs.functions.size() == 1);
    CHECK(stubs.functions[0].name == StringView("add"));
    CHECK(stubs.functions[0].is_pub == false);
    CHECK(stubs.functions[0].is_native == false);
    CHECK(stubs.functions[0].has_body == true);
    CHECK(stubs.functions[0].params.size() == 2);
    CHECK(stubs.functions[0].params[0].name == StringView("a"));
    CHECK(stubs.functions[0].params[1].name == StringView("b"));
    CHECK(stubs.functions[0].return_type.name == StringView("i32"));
}

TEST_CASE("Indexer: Pub function") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("pub fun greet(): string { return \"hi\"; }", allocator);

    REQUIRE(stubs.functions.size() == 1);
    CHECK(stubs.functions[0].name == StringView("greet"));
    CHECK(stubs.functions[0].is_pub == true);
    CHECK(stubs.functions[0].is_native == false);
}

TEST_CASE("Indexer: Native function") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("native fun print(s: string);", allocator);

    REQUIRE(stubs.functions.size() == 1);
    CHECK(stubs.functions[0].name == StringView("print"));
    CHECK(stubs.functions[0].is_pub == false);
    CHECK(stubs.functions[0].is_native == true);
    CHECK(stubs.functions[0].has_body == false);
}

TEST_CASE("Indexer: Pub native function") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("pub native fun sqrt(x: f64): f64;", allocator);

    REQUIRE(stubs.functions.size() == 1);
    CHECK(stubs.functions[0].name == StringView("sqrt"));
    CHECK(stubs.functions[0].is_pub == true);
    CHECK(stubs.functions[0].is_native == true);
}

TEST_CASE("Indexer: Method declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun Vec2.len(): f32 { return 0.0; }", allocator);

    REQUIRE(stubs.methods.size() == 1);
    CHECK(stubs.methods[0].struct_name == StringView("Vec2"));
    CHECK(stubs.methods[0].method_name == StringView("len"));
    CHECK(stubs.methods[0].has_body == true);
}

TEST_CASE("Indexer: Method with trait") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun Vec2.to_string(): string for Printable { return \"vec\"; }", allocator);

    REQUIRE(stubs.methods.size() == 1);
    CHECK(stubs.methods[0].struct_name == StringView("Vec2"));
    CHECK(stubs.methods[0].method_name == StringView("to_string"));
    CHECK(stubs.methods[0].trait_name == StringView("Printable"));
}

TEST_CASE("Indexer: Constructor declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun new Point(x: f32, y: f32) { }", allocator);

    REQUIRE(stubs.constructors.size() == 1);
    CHECK(stubs.constructors[0].struct_name == StringView("Point"));
    CHECK(stubs.constructors[0].params.size() == 2);
}

TEST_CASE("Indexer: Named constructor") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun new Point.origin() { }", allocator);

    REQUIRE(stubs.constructors.size() == 1);
    CHECK(stubs.constructors[0].struct_name == StringView("Point"));
    CHECK(stubs.constructors[0].constructor_name == StringView("origin"));
}

TEST_CASE("Indexer: Destructor declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun delete Resource() { }", allocator);

    REQUIRE(stubs.destructors.size() == 1);
    CHECK(stubs.destructors[0].struct_name == StringView("Resource"));
}

TEST_CASE("Indexer: Struct with fields") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source(
        "struct Point {\n"
        "    x: f32;\n"
        "    y: f32;\n"
        "}",
        allocator);

    REQUIRE(stubs.structs.size() == 1);
    CHECK(stubs.structs[0].name == StringView("Point"));
    CHECK(stubs.structs[0].is_pub == false);
    CHECK(stubs.structs[0].fields.size() == 2);
    CHECK(stubs.structs[0].fields[0].name == StringView("x"));
    CHECK(stubs.structs[0].fields[0].type.name == StringView("f32"));
    CHECK(stubs.structs[0].fields[1].name == StringView("y"));
}

TEST_CASE("Indexer: Pub struct") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("pub struct Rect { w: f32; h: f32; }", allocator);

    REQUIRE(stubs.structs.size() == 1);
    CHECK(stubs.structs[0].name == StringView("Rect"));
    CHECK(stubs.structs[0].is_pub == true);
}

TEST_CASE("Indexer: Struct with parent") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("struct Cat : Animal { name: string; }", allocator);

    REQUIRE(stubs.structs.size() == 1);
    CHECK(stubs.structs[0].name == StringView("Cat"));
    CHECK(stubs.structs[0].parent_name == StringView("Animal"));
}

TEST_CASE("Indexer: Struct with pub field") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("struct Foo { pub x: i32; y: i32; }", allocator);

    REQUIRE(stubs.structs.size() == 1);
    REQUIRE(stubs.structs[0].fields.size() == 2);
    CHECK(stubs.structs[0].fields[0].name == StringView("x"));
    CHECK(stubs.structs[0].fields[0].is_pub == true);
    CHECK(stubs.structs[0].fields[1].name == StringView("y"));
    CHECK(stubs.structs[0].fields[1].is_pub == false);
}

TEST_CASE("Indexer: Enum with variants") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("enum Color { Red, Green, Blue }", allocator);

    REQUIRE(stubs.enums.size() == 1);
    CHECK(stubs.enums[0].name == StringView("Color"));
    CHECK(stubs.enums[0].variants.size() == 3);
    CHECK(stubs.enums[0].variants[0].name == StringView("Red"));
    CHECK(stubs.enums[0].variants[1].name == StringView("Green"));
    CHECK(stubs.enums[0].variants[2].name == StringView("Blue"));
}

TEST_CASE("Indexer: Trait declaration") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("trait Printable;", allocator);

    REQUIRE(stubs.traits.size() == 1);
    CHECK(stubs.traits[0].name == StringView("Printable"));
    CHECK(stubs.traits[0].is_pub == false);
}

TEST_CASE("Indexer: Trait with parent") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("trait Drawable : Printable;", allocator);

    REQUIRE(stubs.traits.size() == 1);
    CHECK(stubs.traits[0].name == StringView("Drawable"));
    CHECK(stubs.traits[0].parent_name == StringView("Printable"));
}

TEST_CASE("Indexer: Simple import") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("import math;", allocator);

    REQUIRE(stubs.imports.size() == 1);
    CHECK(stubs.imports[0].module_path == StringView("math"));
    CHECK(stubs.imports[0].is_from_import == false);
}

TEST_CASE("Indexer: From import") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("from math import sin, cos;", allocator);

    REQUIRE(stubs.imports.size() == 1);
    CHECK(stubs.imports[0].module_path == StringView("math"));
    CHECK(stubs.imports[0].is_from_import == true);
    CHECK(stubs.imports[0].imported_names.size() == 2);
    CHECK(stubs.imports[0].imported_names[0] == StringView("sin"));
    CHECK(stubs.imports[0].imported_names[1] == StringView("cos"));
}

TEST_CASE("Indexer: Source with errors still extracts valid declarations") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source(
        "fun valid_func(): i32 { return 1; }\n"
        "var broken =\n"  // incomplete
        "struct Point { x: f32; y: f32; }",
        allocator);

    // Should still get the function and struct
    CHECK(stubs.functions.size() >= 1);
    CHECK(stubs.structs.size() >= 1);
}

TEST_CASE("Indexer: Multiple declarations") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source(
        "import math;\n"
        "var pi: f64 = 3.14;\n"
        "trait Printable;\n"
        "enum Color { Red, Green }\n"
        "struct Point { x: f32; y: f32; }\n"
        "fun add(a: i32, b: i32): i32 { return a + b; }\n"
        "fun Point.len(): f32 { return 0.0; }",
        allocator);

    CHECK(stubs.imports.size() == 1);
    CHECK(stubs.globals.size() == 1);
    CHECK(stubs.traits.size() == 1);
    CHECK(stubs.enums.size() == 1);
    CHECK(stubs.structs.size() == 1);
    CHECK(stubs.functions.size() == 1);
    CHECK(stubs.methods.size() == 1);
}

TEST_CASE("Indexer: Field with default value") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("struct Conf { timeout: i32 = 30; }", allocator);

    REQUIRE(stubs.structs.size() == 1);
    REQUIRE(stubs.structs[0].fields.size() == 1);
    CHECK(stubs.structs[0].fields[0].has_default == true);
}

TEST_CASE("Indexer: Function without return type") {
    BumpAllocator allocator(4096);
    FileStubs stubs = index_source("fun greet() { }", allocator);

    REQUIRE(stubs.functions.size() == 1);
    CHECK(stubs.functions[0].name == StringView("greet"));
    CHECK(stubs.functions[0].params.size() == 0);
}

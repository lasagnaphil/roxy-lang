#include "roxy/core/doctest/doctest.h"

#include "roxy/lsp/cst_lowering.hpp"
#include "roxy/lsp/lsp_parser.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse source to CST then lower to AST
static Program* parse_and_lower(const char* source, BumpAllocator& parse_allocator, BumpAllocator& ast_allocator) {
    u32 length = static_cast<u32>(strlen(source));
    Lexer lexer(source, length);
    LspParser parser(lexer, parse_allocator);
    SyntaxTree tree = parser.parse();

    CstLowering lowering(ast_allocator);
    return lowering.lower(tree.root);
}

// --- CstLowering unit tests ---

TEST_CASE("CstLowering: VarDecl with type annotation") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("var x: i32;", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclVar);
    CHECK(decl->var_decl.name == StringView("x"));
    REQUIRE(decl->var_decl.type != nullptr);
    CHECK(decl->var_decl.type->name == StringView("i32"));
    CHECK(decl->var_decl.initializer == nullptr);
}

TEST_CASE("CstLowering: VarDecl with initializer (struct literal)") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("var p = Point { x = 1 };", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclVar);
    CHECK(decl->var_decl.name == StringView("p"));
    CHECK(decl->var_decl.type == nullptr);
    REQUIRE(decl->var_decl.initializer != nullptr);
    CHECK(decl->var_decl.initializer->kind == AstKind::ExprStructLiteral);
    CHECK(decl->var_decl.initializer->struct_literal.type_name == StringView("Point"));
}

TEST_CASE("CstLowering: FunDecl with params and return type") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun add(a: i32, b: i32): i32 { return a + b; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclFun);
    CHECK(decl->fun_decl.name == StringView("add"));
    REQUIRE(decl->fun_decl.params.size() == 2);
    CHECK(decl->fun_decl.params[0].name == StringView("a"));
    CHECK(decl->fun_decl.params[0].type->name == StringView("i32"));
    CHECK(decl->fun_decl.params[1].name == StringView("b"));
    REQUIRE(decl->fun_decl.return_type != nullptr);
    CHECK(decl->fun_decl.return_type->name == StringView("i32"));
    CHECK(decl->fun_decl.body != nullptr);
}

TEST_CASE("CstLowering: MethodDecl") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun Point.length(): f32 { return 0.0; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclMethod);
    CHECK(decl->method_decl.struct_name == StringView("Point"));
    CHECK(decl->method_decl.name == StringView("length"));
    REQUIRE(decl->method_decl.return_type != nullptr);
    CHECK(decl->method_decl.return_type->name == StringView("f32"));
    CHECK(decl->method_decl.body != nullptr);
}

TEST_CASE("CstLowering: GetExpr") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun main() { var x = p.y; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* fun_decl = program->declarations[0];
    REQUIRE(fun_decl->kind == AstKind::DeclFun);
    REQUIRE(fun_decl->fun_decl.body != nullptr);
    REQUIRE(fun_decl->fun_decl.body->kind == AstKind::StmtBlock);
    REQUIRE(fun_decl->fun_decl.body->block.declarations.size() == 1);

    Decl* var_decl = fun_decl->fun_decl.body->block.declarations[0];
    REQUIRE(var_decl->kind == AstKind::DeclVar);
    REQUIRE(var_decl->var_decl.initializer != nullptr);
    CHECK(var_decl->var_decl.initializer->kind == AstKind::ExprGet);
    CHECK(var_decl->var_decl.initializer->get.name == StringView("y"));
    REQUIRE(var_decl->var_decl.initializer->get.object != nullptr);
    CHECK(var_decl->var_decl.initializer->get.object->kind == AstKind::ExprIdentifier);
    CHECK(var_decl->var_decl.initializer->get.object->identifier.name == StringView("p"));
}

TEST_CASE("CstLowering: CallExpr") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun main() { foo(1, 2); }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* fun_decl = program->declarations[0];
    REQUIRE(fun_decl->fun_decl.body != nullptr);
    REQUIRE(fun_decl->fun_decl.body->block.declarations.size() == 1);

    Decl* expr_decl = fun_decl->fun_decl.body->block.declarations[0];
    // ExprStmt wraps an ExprCall
    REQUIRE(expr_decl->kind == AstKind::StmtExpr);
    REQUIRE(expr_decl->stmt.expr_stmt.expr != nullptr);
    CHECK(expr_decl->stmt.expr_stmt.expr->kind == AstKind::ExprCall);
    CHECK(expr_decl->stmt.expr_stmt.expr->call.arguments.size() == 2);
}

TEST_CASE("CstLowering: BlockStmt with nested declarations") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun main() { var a: i32; var b: f32; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* fun_decl = program->declarations[0];
    REQUIRE(fun_decl->fun_decl.body != nullptr);
    CHECK(fun_decl->fun_decl.body->block.declarations.size() == 2);
    CHECK(fun_decl->fun_decl.body->block.declarations[0]->kind == AstKind::DeclVar);
    CHECK(fun_decl->fun_decl.body->block.declarations[1]->kind == AstKind::DeclVar);
}

TEST_CASE("CstLowering: Error recovery produces nullptr") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    // This will produce error nodes in the CST
    Program* program = parse_and_lower("fun main() { ??? }", parse_alloc, ast_alloc);

    // Should not crash, program should still be created
    REQUIRE(program != nullptr);
}

TEST_CASE("CstLowering: StructDecl with fields") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("struct Point { x: f32; y: f32; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclStruct);
    CHECK(decl->struct_decl.name == StringView("Point"));
    REQUIRE(decl->struct_decl.fields.size() == 2);
    CHECK(decl->struct_decl.fields[0].name == StringView("x"));
    CHECK(decl->struct_decl.fields[0].type->name == StringView("f32"));
    CHECK(decl->struct_decl.fields[1].name == StringView("y"));
}

TEST_CASE("CstLowering: ConstructorDecl") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun new Point(x: f32) { }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    REQUIRE(program->declarations.size() == 1);

    Decl* decl = program->declarations[0];
    REQUIRE(decl->kind == AstKind::DeclConstructor);
    CHECK(decl->constructor_decl.struct_name == StringView("Point"));
    REQUIRE(decl->constructor_decl.params.size() == 1);
}

TEST_CASE("CstLowering: BinaryExpr") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun main() { var x = 1 + 2; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    Decl* fun_decl = program->declarations[0];
    Decl* var_decl = fun_decl->fun_decl.body->block.declarations[0];
    REQUIRE(var_decl->var_decl.initializer != nullptr);
    CHECK(var_decl->var_decl.initializer->kind == AstKind::ExprBinary);
    CHECK(var_decl->var_decl.initializer->binary.op == BinaryOp::Add);
}

TEST_CASE("CstLowering: SelfExpr") {
    BumpAllocator parse_alloc(4096);
    BumpAllocator ast_alloc(4096);
    Program* program = parse_and_lower("fun Point.get_x(): f32 { return self.x; }", parse_alloc, ast_alloc);

    REQUIRE(program != nullptr);
    Decl* method_decl = program->declarations[0];
    REQUIRE(method_decl->kind == AstKind::DeclMethod);
    REQUIRE(method_decl->method_decl.body != nullptr);

    // return stmt -> ExprGet(self, "x")
    Decl* return_decl = method_decl->method_decl.body->block.declarations[0];
    REQUIRE(return_decl->kind == AstKind::StmtReturn);
    REQUIRE(return_decl->stmt.return_stmt.value != nullptr);
    CHECK(return_decl->stmt.return_stmt.value->kind == AstKind::ExprGet);
    CHECK(return_decl->stmt.return_stmt.value->get.name == StringView("x"));
    REQUIRE(return_decl->stmt.return_stmt.value->get.object != nullptr);
    CHECK(return_decl->stmt.return_stmt.value->get.object->kind == AstKind::ExprThis);
}

#include "roxy/core/doctest/doctest.h"

#include "roxy/compiler/parser.hpp"
#include "roxy/core/bump_allocator.hpp"

#include <cstring>

using namespace rx;

// Helper to parse source and return program
Program* parse_source(const char* source, BumpAllocator& allocator, ParseError* out_error = nullptr) {
    Lexer lexer(source, (u32)strlen(source));
    Parser parser(lexer, allocator);
    Program* program = parser.parse();
    if (parser.has_error() && out_error) {
        *out_error = parser.error();
    }
    return program;
}

// Helper to check identifier name
bool check_identifier(StringView sv, const char* expected) {
    return sv == expected;
}

TEST_CASE("Parser: Literal Expressions") {
    BumpAllocator allocator(4096);

    SUBCASE("Nil literal") {
        Program* program = parse_source("nil;", allocator);
        REQUIRE(program != nullptr);
        REQUIRE(program->declarations.size() == 1);
        CHECK(program->declarations[0]->kind == AstKind::StmtExpr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.expr_stmt.expr->kind == AstKind::ExprLiteral);
        CHECK(stmt.expr_stmt.expr->literal.literal_kind == LiteralKind::Nil);
    }

    SUBCASE("Boolean literals") {
        Program* program = parse_source("true; false;", allocator);
        REQUIRE(program != nullptr);
        REQUIRE(program->declarations.size() == 2);

        auto& expr1 = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr1->literal.literal_kind == LiteralKind::Bool);
        CHECK(expr1->literal.bool_value == true);

        auto& expr2 = program->declarations[1]->stmt.expr_stmt.expr;
        CHECK(expr2->literal.literal_kind == LiteralKind::Bool);
        CHECK(expr2->literal.bool_value == false);
    }

    SUBCASE("Integer literal") {
        Program* program = parse_source("42;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->literal.literal_kind == LiteralKind::I32);
        CHECK(expr->literal.int_value == 42);
    }

    SUBCASE("Float literal") {
        Program* program = parse_source("3.14;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->literal.literal_kind == LiteralKind::F64);
        CHECK(expr->literal.float_value == doctest::Approx(3.14));
    }

    SUBCASE("String literal") {
        Program* program = parse_source("\"hello\";", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->literal.literal_kind == LiteralKind::String);
        // Parser strips quotes and processes escape sequences
        CHECK(check_identifier(expr->literal.string_value, "hello"));
    }
}

TEST_CASE("Parser: Identifier Expression") {
    BumpAllocator allocator(4096);

    Program* program = parse_source("foo;", allocator);
    REQUIRE(program != nullptr);
    auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
    CHECK(expr->kind == AstKind::ExprIdentifier);
    CHECK(check_identifier(expr->identifier.name, "foo"));
}

TEST_CASE("Parser: Unary Expressions") {
    BumpAllocator allocator(4096);

    SUBCASE("Negation") {
        Program* program = parse_source("-42;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprUnary);
        CHECK(expr->unary.op == UnaryOp::Negate);
        CHECK(expr->unary.operand->kind == AstKind::ExprLiteral);
    }

    SUBCASE("Logical not") {
        Program* program = parse_source("!true;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprUnary);
        CHECK(expr->unary.op == UnaryOp::Not);
    }

    SUBCASE("Bitwise not") {
        Program* program = parse_source("~x;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprUnary);
        CHECK(expr->unary.op == UnaryOp::BitNot);
    }
}

TEST_CASE("Parser: Binary Expressions") {
    BumpAllocator allocator(4096);

    SUBCASE("Arithmetic") {
        Program* program = parse_source("1 + 2;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprBinary);
        CHECK(expr->binary.op == BinaryOp::Add);
        CHECK(expr->binary.left->literal.int_value == 1);
        CHECK(expr->binary.right->literal.int_value == 2);
    }

    SUBCASE("All arithmetic operators") {
        const char* sources[] = {"a - b;", "a * b;", "a / b;", "a % b;"};
        BinaryOp ops[] = {BinaryOp::Sub, BinaryOp::Mul, BinaryOp::Div, BinaryOp::Mod};

        for (int i = 0; i < 4; i++) {
            BumpAllocator alloc(4096);
            Program* prog = parse_source(sources[i], alloc);
            REQUIRE(prog != nullptr);
            CHECK(prog->declarations[0]->stmt.expr_stmt.expr->binary.op == ops[i]);
        }
    }

    SUBCASE("Comparison operators") {
        const char* sources[] = {"a < b;", "a <= b;", "a > b;", "a >= b;"};
        BinaryOp ops[] = {BinaryOp::Less, BinaryOp::LessEq, BinaryOp::Greater, BinaryOp::GreaterEq};

        for (int i = 0; i < 4; i++) {
            BumpAllocator alloc(4096);
            Program* prog = parse_source(sources[i], alloc);
            REQUIRE(prog != nullptr);
            CHECK(prog->declarations[0]->stmt.expr_stmt.expr->binary.op == ops[i]);
        }
    }

    SUBCASE("Equality operators") {
        BumpAllocator alloc1(4096);
        Program* prog1 = parse_source("a == b;", alloc1);
        REQUIRE(prog1 != nullptr);
        CHECK(prog1->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::Equal);

        BumpAllocator alloc2(4096);
        Program* prog2 = parse_source("a != b;", alloc2);
        REQUIRE(prog2 != nullptr);
        CHECK(prog2->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::NotEqual);
    }

    SUBCASE("Logical operators") {
        BumpAllocator alloc1(4096);
        Program* prog1 = parse_source("a && b;", alloc1);
        REQUIRE(prog1 != nullptr);
        CHECK(prog1->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::And);

        BumpAllocator alloc2(4096);
        Program* prog2 = parse_source("a || b;", alloc2);
        REQUIRE(prog2 != nullptr);
        CHECK(prog2->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::Or);
    }

    SUBCASE("Bitwise operators") {
        BumpAllocator alloc1(4096);
        Program* prog1 = parse_source("a & b;", alloc1);
        REQUIRE(prog1 != nullptr);
        CHECK(prog1->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::BitAnd);

        BumpAllocator alloc2(4096);
        Program* prog2 = parse_source("a | b;", alloc2);
        REQUIRE(prog2 != nullptr);
        CHECK(prog2->declarations[0]->stmt.expr_stmt.expr->binary.op == BinaryOp::BitOr);
    }
}

TEST_CASE("Parser: Operator Precedence") {
    BumpAllocator allocator(4096);

    SUBCASE("Multiplication before addition") {
        // 1 + 2 * 3 should be 1 + (2 * 3)
        Program* program = parse_source("1 + 2 * 3;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->binary.op == BinaryOp::Add);
        CHECK(expr->binary.left->literal.int_value == 1);
        CHECK(expr->binary.right->binary.op == BinaryOp::Mul);
    }

    SUBCASE("Comparison before equality") {
        // a < b == c should be (a < b) == c
        Program* program = parse_source("a < b == c;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->binary.op == BinaryOp::Equal);
        CHECK(expr->binary.left->binary.op == BinaryOp::Less);
    }

    SUBCASE("Unary before binary") {
        // -a + b should be (-a) + b
        Program* program = parse_source("-a + b;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->binary.op == BinaryOp::Add);
        CHECK(expr->binary.left->kind == AstKind::ExprUnary);
    }

    SUBCASE("Grouping overrides precedence") {
        // (1 + 2) * 3 should be (1 + 2) * 3
        Program* program = parse_source("(1 + 2) * 3;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->binary.op == BinaryOp::Mul);
        CHECK(expr->binary.left->kind == AstKind::ExprGrouping);
        CHECK(expr->binary.left->grouping.expr->binary.op == BinaryOp::Add);
    }
}

TEST_CASE("Parser: Ternary Expression") {
    BumpAllocator allocator(4096);

    Program* program = parse_source("a ? b : c;", allocator);
    REQUIRE(program != nullptr);
    auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
    CHECK(expr->kind == AstKind::ExprTernary);
    CHECK(expr->ternary.condition->kind == AstKind::ExprIdentifier);
    CHECK(expr->ternary.then_expr->kind == AstKind::ExprIdentifier);
    CHECK(expr->ternary.else_expr->kind == AstKind::ExprIdentifier);
}

TEST_CASE("Parser: Call Expression") {
    BumpAllocator allocator(4096);

    SUBCASE("No arguments") {
        Program* program = parse_source("foo();", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprCall);
        CHECK(expr->call.arguments.size() == 0);
    }

    SUBCASE("With arguments") {
        Program* program = parse_source("foo(1, 2, 3);", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprCall);
        CHECK(expr->call.arguments.size() == 3);
    }

    SUBCASE("Chained calls") {
        Program* program = parse_source("foo()();", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprCall);
        CHECK(expr->call.callee->kind == AstKind::ExprCall);
    }
}

TEST_CASE("Parser: Member Access") {
    BumpAllocator allocator(4096);

    SUBCASE("Dot access") {
        Program* program = parse_source("obj.field;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprGet);
        CHECK(check_identifier(expr->get.name, "field"));
    }

    SUBCASE("Static access") {
        Program* program = parse_source("Type::method;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprStaticGet);
        CHECK(check_identifier(expr->static_get.type_name, "Type"));
        CHECK(check_identifier(expr->static_get.member_name, "method"));
    }

    SUBCASE("Index access") {
        Program* program = parse_source("arr[0];", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprIndex);
    }

    SUBCASE("Chained access") {
        Program* program = parse_source("a.b.c;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprGet);
        CHECK(expr->get.object->kind == AstKind::ExprGet);
    }
}

TEST_CASE("Parser: Assignment Expression") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple assignment") {
        Program* program = parse_source("x = 5;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprAssign);
        CHECK(expr->assign.op == AssignOp::Assign);
    }

    SUBCASE("Compound assignments") {
        const char* sources[] = {"x += 1;", "x -= 1;", "x *= 2;", "x /= 2;", "x %= 3;"};
        AssignOp ops[] = {AssignOp::AddAssign, AssignOp::SubAssign, AssignOp::MulAssign,
                         AssignOp::DivAssign, AssignOp::ModAssign};

        for (int i = 0; i < 5; i++) {
            BumpAllocator alloc(4096);
            Program* prog = parse_source(sources[i], alloc);
            REQUIRE(prog != nullptr);
            CHECK(prog->declarations[0]->stmt.expr_stmt.expr->assign.op == ops[i]);
        }
    }
}

TEST_CASE("Parser: Self and Super") {
    BumpAllocator allocator(4096);

    SUBCASE("Self expression") {
        Program* program = parse_source("self;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprThis);
    }

    SUBCASE("Super expression") {
        Program* program = parse_source("super.method;", allocator);
        REQUIRE(program != nullptr);
        auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
        CHECK(expr->kind == AstKind::ExprSuper);
        CHECK(check_identifier(expr->super_expr.method_name, "method"));
    }
}

TEST_CASE("Parser: Constructor Call Expression") {
    BumpAllocator allocator(4096);

    // Constructor calls use the same syntax as function calls: Type(args)
    Program* program = parse_source("Point(1, 2);", allocator);
    REQUIRE(program != nullptr);
    auto& expr = program->declarations[0]->stmt.expr_stmt.expr;
    CHECK(expr->kind == AstKind::ExprCall);
    CHECK(expr->call.callee->kind == AstKind::ExprIdentifier);
    CHECK(check_identifier(expr->call.callee->identifier.name, "Point"));
    CHECK(expr->call.arguments.size() == 2);
}

TEST_CASE("Parser: Block Statement") {
    BumpAllocator allocator(4096);

    Program* program = parse_source("{ x; y; z; }", allocator);
    REQUIRE(program != nullptr);
    auto& stmt = program->declarations[0]->stmt;
    CHECK(stmt.kind == AstKind::StmtBlock);
    CHECK(stmt.block.declarations.size() == 3);
}

TEST_CASE("Parser: If Statement") {
    BumpAllocator allocator(4096);

    SUBCASE("Without else") {
        Program* program = parse_source("if (x) y;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtIf);
        CHECK(stmt.if_stmt.else_branch == nullptr);
    }

    SUBCASE("With else") {
        Program* program = parse_source("if (x) y; else z;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtIf);
        CHECK(stmt.if_stmt.else_branch != nullptr);
    }

    SUBCASE("Else if chain") {
        Program* program = parse_source("if (a) x; else if (b) y; else z;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtIf);
        CHECK(stmt.if_stmt.else_branch->kind == AstKind::StmtIf);
    }
}

TEST_CASE("Parser: While Statement") {
    BumpAllocator allocator(4096);

    Program* program = parse_source("while (x) y;", allocator);
    REQUIRE(program != nullptr);
    auto& stmt = program->declarations[0]->stmt;
    CHECK(stmt.kind == AstKind::StmtWhile);
}

TEST_CASE("Parser: For Statement") {
    BumpAllocator allocator(4096);

    SUBCASE("Full for loop") {
        Program* program = parse_source("for (var i: i32 = 0; i < 10; i += 1) x;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtFor);
        CHECK(stmt.for_stmt.initializer != nullptr);
        CHECK(stmt.for_stmt.condition != nullptr);
        CHECK(stmt.for_stmt.increment != nullptr);
    }

    SUBCASE("Empty for loop") {
        Program* program = parse_source("for (;;) x;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtFor);
        CHECK(stmt.for_stmt.initializer == nullptr);
        CHECK(stmt.for_stmt.condition == nullptr);
        CHECK(stmt.for_stmt.increment == nullptr);
    }
}

TEST_CASE("Parser: Control Flow Statements") {
    BumpAllocator allocator(4096);

    SUBCASE("Return with value") {
        Program* program = parse_source("return 42;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtReturn);
        CHECK(stmt.return_stmt.value != nullptr);
    }

    SUBCASE("Return without value") {
        Program* program = parse_source("return;", allocator);
        REQUIRE(program != nullptr);
        auto& stmt = program->declarations[0]->stmt;
        CHECK(stmt.kind == AstKind::StmtReturn);
        CHECK(stmt.return_stmt.value == nullptr);
    }

    SUBCASE("Break") {
        Program* program = parse_source("break;", allocator);
        REQUIRE(program != nullptr);
        CHECK(program->declarations[0]->stmt.kind == AstKind::StmtBreak);
    }

    SUBCASE("Continue") {
        Program* program = parse_source("continue;", allocator);
        REQUIRE(program != nullptr);
        CHECK(program->declarations[0]->stmt.kind == AstKind::StmtContinue);
    }
}

TEST_CASE("Parser: Delete Statement") {
    BumpAllocator allocator(4096);

    Program* program = parse_source("delete obj;", allocator);
    REQUIRE(program != nullptr);
    auto& stmt = program->declarations[0]->stmt;
    CHECK(stmt.kind == AstKind::StmtDelete);
}

TEST_CASE("Parser: Variable Declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("With type and initializer") {
        Program* program = parse_source("var x: i32 = 5;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclVar);
        CHECK(check_identifier(decl->var_decl.name, "x"));
        CHECK(decl->var_decl.type != nullptr);
        CHECK(decl->var_decl.initializer != nullptr);
    }

    SUBCASE("With type inference") {
        Program* program = parse_source("var x = 5;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclVar);
        CHECK(decl->var_decl.type == nullptr);
        CHECK(decl->var_decl.initializer != nullptr);
    }

    SUBCASE("Without initializer") {
        Program* program = parse_source("var x: i32;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->var_decl.initializer == nullptr);
    }

    SUBCASE("Public variable") {
        Program* program = parse_source("pub var x: i32 = 0;", allocator);
        REQUIRE(program != nullptr);
        CHECK(program->declarations[0]->var_decl.is_pub == true);
    }
}

TEST_CASE("Parser: Function Declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple function") {
        Program* program = parse_source("fun add(a: i32, b: i32): i32 { return a + b; }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclFun);
        CHECK(check_identifier(decl->fun_decl.name, "add"));
        CHECK(decl->fun_decl.params.size() == 2);
        CHECK(decl->fun_decl.return_type != nullptr);
        CHECK(decl->fun_decl.body != nullptr);
    }

    SUBCASE("Function without return type") {
        Program* program = parse_source("fun doSomething() { x; }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->fun_decl.return_type == nullptr);
    }

    SUBCASE("Native function") {
        Program* program = parse_source("native fun print(msg: string);", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->fun_decl.is_native == true);
        CHECK(decl->fun_decl.body == nullptr);
    }

    SUBCASE("Function with out parameter") {
        Program* program = parse_source("fun swap(a: out i32, b: inout i32) { }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->fun_decl.params[0].modifier == ParamModifier::Out);
        CHECK(decl->fun_decl.params[1].modifier == ParamModifier::Inout);
    }

    SUBCASE("Public function") {
        Program* program = parse_source("pub fun foo() { }", allocator);
        REQUIRE(program != nullptr);
        CHECK(program->declarations[0]->fun_decl.is_pub == true);
    }
}

TEST_CASE("Parser: Struct Declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple struct") {
        Program* program = parse_source(
            "struct Point {\n"
            "    x: f32;\n"
            "    y: f32;\n"
            "}", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclStruct);
        CHECK(check_identifier(decl->struct_decl.name, "Point"));
        CHECK(decl->struct_decl.fields.size() == 2);
    }

    SUBCASE("Struct with parent") {
        Program* program = parse_source("struct Child : Parent { }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(check_identifier(decl->struct_decl.parent_name, "Parent"));
    }

    SUBCASE("Struct with methods") {
        Program* program = parse_source(
            "struct Point {\n"
            "    x: f32;\n"
            "    fun length(): f32 { return 0.0; }\n"
            "}", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->struct_decl.fields.size() == 1);
        CHECK(decl->struct_decl.methods.size() == 1);
    }

    SUBCASE("Field with default value") {
        Program* program = parse_source("struct Foo { x: i32 = 0; }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->struct_decl.fields[0].default_value != nullptr);
    }

    SUBCASE("Public struct and members") {
        Program* program = parse_source(
            "pub struct Foo {\n"
            "    pub x: i32;\n"
            "    pub fun bar() { }\n"
            "}", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->struct_decl.is_pub == true);
        CHECK(decl->struct_decl.fields[0].is_pub == true);
        CHECK(decl->struct_decl.methods[0]->is_pub == true);
    }
}

TEST_CASE("Parser: Enum Declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple enum") {
        Program* program = parse_source("enum Color { Red, Green, Blue }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclEnum);
        CHECK(check_identifier(decl->enum_decl.name, "Color"));
        CHECK(decl->enum_decl.variants.size() == 3);
    }

    SUBCASE("Enum with values") {
        Program* program = parse_source("enum Flags { A = 1, B = 2, C = 4 }", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->enum_decl.variants[0].value != nullptr);
        CHECK(decl->enum_decl.variants[0].value->literal.int_value == 1);
    }

    SUBCASE("Trailing comma") {
        Program* program = parse_source("enum E { A, B, }", allocator);
        REQUIRE(program != nullptr);
        CHECK(program->declarations[0]->enum_decl.variants.size() == 2);
    }
}

TEST_CASE("Parser: Import Declaration") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple import") {
        Program* program = parse_source("import std;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->kind == AstKind::DeclImport);
        CHECK(check_identifier(decl->import_decl.module_path, "std"));
        CHECK(decl->import_decl.is_from_import == false);
    }

    SUBCASE("From import") {
        Program* program = parse_source("from std import print, println;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(decl->import_decl.is_from_import == true);
        CHECK(decl->import_decl.names.size() == 2);
    }

    SUBCASE("From import with alias") {
        Program* program = parse_source("from std import print as p;", allocator);
        REQUIRE(program != nullptr);
        auto& decl = program->declarations[0];
        CHECK(check_identifier(decl->import_decl.names[0].alias, "p"));
    }
}

TEST_CASE("Parser: Type Expressions") {
    BumpAllocator allocator(4096);

    SUBCASE("Simple type") {
        Program* program = parse_source("var x: i32;", allocator);
        REQUIRE(program != nullptr);
        auto type = program->declarations[0]->var_decl.type;
        CHECK(check_identifier(type->name, "i32"));
    }

    SUBCASE("Reference types") {
        BumpAllocator alloc1(4096);
        Program* prog1 = parse_source("var x: uniq Foo;", alloc1);
        REQUIRE(prog1 != nullptr);
        CHECK(prog1->declarations[0]->var_decl.type->ref_kind == RefKind::Uniq);

        BumpAllocator alloc2(4096);
        Program* prog2 = parse_source("var x: ref Foo;", alloc2);
        REQUIRE(prog2 != nullptr);
        CHECK(prog2->declarations[0]->var_decl.type->ref_kind == RefKind::Ref);

        BumpAllocator alloc3(4096);
        Program* prog3 = parse_source("var x: weak Foo;", alloc3);
        REQUIRE(prog3 != nullptr);
        CHECK(prog3->declarations[0]->var_decl.type->ref_kind == RefKind::Weak);
    }

    SUBCASE("Array type") {
        Program* program = parse_source("var arr: i32[];", allocator);
        REQUIRE(program != nullptr);
        auto type = program->declarations[0]->var_decl.type;
        CHECK(type->element_type != nullptr);
        CHECK(check_identifier(type->element_type->name, "i32"));
    }
}

TEST_CASE("Parser: Error Cases") {
    BumpAllocator allocator(4096);
    ParseError error;

    SUBCASE("Missing semicolon") {
        Program* program = parse_source("var x: i32", allocator, &error);
        CHECK(program == nullptr);
    }

    SUBCASE("Missing closing paren") {
        Program* program = parse_source("foo(1, 2", allocator, &error);
        CHECK(program == nullptr);
    }

    SUBCASE("Missing closing brace") {
        Program* program = parse_source("{ x;", allocator, &error);
        CHECK(program == nullptr);
    }

    SUBCASE("Expected expression") {
        Program* program = parse_source(";", allocator, &error);
        CHECK(program == nullptr);
    }

    SUBCASE("Invalid native usage") {
        Program* program = parse_source("native var x: i32;", allocator, &error);
        CHECK(program == nullptr);
    }
}

TEST_CASE("Parser: Complete Program") {
    BumpAllocator allocator(4096);

    const char* source = R"(
        struct Point {
            x: f32;
            y: f32;

            fun create(x: f32, y: f32): Point {
                var p: Point;
                p.x = x;
                p.y = y;
                return p;
            }

            fun add(other: ref Point): Point {
                return Point::create(this.x + other.x, this.y + other.y);
            }
        }

        fun main(): i32 {
            var p1 = Point::create(1.0, 2.0);
            var p2 = Point::create(3.0, 4.0);
            var p3 = p1.add(p2);
            return 0;
        }
    )";

    ParseError error;
    Program* program = parse_source(source, allocator, &error);
    if (program == nullptr) {
        MESSAGE("Parse error at line " << error.loc.line << ", column " << error.loc.column << ": " << error.message);
    }
    REQUIRE(program != nullptr);
    CHECK(program->declarations.size() == 2);
    CHECK(program->declarations[0]->kind == AstKind::DeclStruct);
    CHECK(program->declarations[1]->kind == AstKind::DeclFun);
}

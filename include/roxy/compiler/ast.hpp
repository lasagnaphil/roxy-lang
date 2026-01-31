#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/shared/token.hpp"

#include <cstring>

namespace rx {

// Forward declarations
struct Expr;
struct Stmt;
struct Decl;
struct TypeExpr;

enum class AstKind : u8 {
    // Expressions
    ExprLiteral,
    ExprIdentifier,
    ExprUnary,
    ExprBinary,
    ExprTernary,
    ExprCall,
    ExprIndex,
    ExprGet,
    ExprStaticGet,
    ExprAssign,
    ExprGrouping,
    ExprThis,
    ExprSuper,
    ExprNew,
    ExprStructLiteral,

    // Statements
    StmtExpr,
    StmtBlock,
    StmtIf,
    StmtWhile,
    StmtFor,
    StmtReturn,
    StmtBreak,
    StmtContinue,
    StmtDelete,

    // Declarations
    DeclVar,
    DeclFun,
    DeclStruct,
    DeclField,
    DeclEnum,
    DeclImport,
};

enum class LiteralKind : u8 {
    Nil,
    Bool,
    Int,
    Float,
    String,
};

enum class UnaryOp : u8 {
    Negate,     // -
    Not,        // !
    BitNot,     // ~
};

enum class BinaryOp : u8 {
    Add,        // +
    Sub,        // -
    Mul,        // *
    Div,        // /
    Mod,        // %
    Equal,      // ==
    NotEqual,   // !=
    Less,       // <
    LessEq,     // <=
    Greater,    // >
    GreaterEq,  // >=
    And,        // &&
    Or,         // ||
    BitAnd,     // &
    BitOr,      // |
};

enum class AssignOp : u8 {
    Assign,     // =
    AddAssign,  // +=
    SubAssign,  // -=
    MulAssign,  // *=
    DivAssign,  // /=
    ModAssign,  // %=
};

// Parameter modifier for function parameters (also used in call arguments)
enum class ParamModifier : u8 {
    None,
    Out,
    Inout,
};

// Reference kind for type expressions
enum class RefKind : u8 {
    None,   // Value type (no reference wrapper)
    Uniq,   // uniq<T> - owning reference
    Ref,    // ref<T> - borrowed reference
    Weak,   // weak<T> - weak reference
};

// Type expression for type annotations
struct TypeExpr {
    StringView name;
    SourceLocation loc;
    RefKind ref_kind;
    TypeExpr* element_type;  // For array types: element_type[]
};

// Literal expression: nil, true, false, 42, 3.14, "hello"
struct LiteralExpr {
    LiteralKind literal_kind;
    union {
        bool bool_value;
        i64 int_value;
        f64 float_value;
        StringView string_value;
    };
};

// Identifier expression: foo, bar
struct IdentifierExpr {
    StringView name;
};

// Unary expression: -x, !x, ~x
struct UnaryExpr {
    UnaryOp op;
    Expr* operand;
};

// Binary expression: a + b, x < y
struct BinaryExpr {
    BinaryOp op;
    Expr* left;
    Expr* right;
};

// Ternary expression: cond ? then_expr : else_expr
struct TernaryExpr {
    Expr* condition;
    Expr* then_expr;
    Expr* else_expr;
};

// Call argument with optional modifier (for out/inout parameters)
struct CallArg {
    Expr* expr;
    ParamModifier modifier;
    SourceLocation modifier_loc;
};

// Call expression: foo(a, b, c) or foo(inout x, out y)
struct CallExpr {
    Expr* callee;
    Span<CallArg> arguments;
};

// Index expression: arr[i]
struct IndexExpr {
    Expr* object;
    Expr* index;
};

// Get expression: obj.field
struct GetExpr {
    Expr* object;
    StringView name;
};

// Static get expression: Type::method
struct StaticGetExpr {
    StringView type_name;
    StringView member_name;
};

// Assignment expression: x = 5, x += 1
struct AssignExpr {
    AssignOp op;
    Expr* target;
    Expr* value;
};

// Grouping expression: (expr)
struct GroupingExpr {
    Expr* expr;
};

// This expression: this
struct ThisExpr {
    // No additional data needed
};

// Super expression: super.method
struct SuperExpr {
    StringView method_name;
};

// New expression: new Type(args)
struct NewExpr {
    TypeExpr* type;
    Span<Expr*> arguments;
};

// Field initializer for struct literals
struct FieldInit {
    StringView name;
    Expr* value;
    SourceLocation loc;
};

// Struct literal expression: Point { x = 1, y = 2 }
struct StructLiteralExpr {
    StringView type_name;
    Span<FieldInit> fields;
};

// Forward declaration
struct Type;

// Expression node
struct Expr {
    AstKind kind;
    SourceLocation loc;
    Type* resolved_type;  // Set by semantic analysis, nullptr before analysis
    union {
        LiteralExpr literal;
        IdentifierExpr identifier;
        UnaryExpr unary;
        BinaryExpr binary;
        TernaryExpr ternary;
        CallExpr call;
        IndexExpr index;
        GetExpr get;
        StaticGetExpr static_get;
        AssignExpr assign;
        GroupingExpr grouping;
        ThisExpr this_expr;
        SuperExpr super_expr;
        NewExpr new_expr;
        StructLiteralExpr struct_literal;
    };

    Expr() : kind(AstKind::ExprLiteral), loc{0, 0, 0}, resolved_type(nullptr) {
        memset(&literal, 0, sizeof(literal));
    }
    ~Expr() {}
};

// Expression statement: expr;
struct ExprStmt {
    Expr* expr;
};

// Block statement: { stmts }
struct BlockStmt {
    Span<Decl*> declarations;
};

// If statement: if (cond) then_stmt else else_stmt
struct IfStmt {
    Expr* condition;
    Stmt* then_branch;
    Stmt* else_branch;  // nullptr if no else
};

// While statement: while (cond) body
struct WhileStmt {
    Expr* condition;
    Stmt* body;
};

// For statement: for (init; cond; incr) body
struct ForStmt {
    Decl* initializer;  // var decl or expr stmt, nullptr if omitted
    Expr* condition;    // nullptr if omitted
    Expr* increment;    // nullptr if omitted
    Stmt* body;
};

// Return statement: return expr;
struct ReturnStmt {
    Expr* value;  // nullptr if just "return;"
};

// Break statement: break;
struct BreakStmt {
    // No additional data
};

// Continue statement: continue;
struct ContinueStmt {
    // No additional data
};

// Delete statement: delete expr;
struct DeleteStmt {
    Expr* expr;
};

// Statement node
struct Stmt {
    AstKind kind;
    SourceLocation loc;
    union {
        ExprStmt expr_stmt;
        BlockStmt block;
        IfStmt if_stmt;
        WhileStmt while_stmt;
        ForStmt for_stmt;
        ReturnStmt return_stmt;
        BreakStmt break_stmt;
        ContinueStmt continue_stmt;
        DeleteStmt delete_stmt;
    };

    Stmt() : kind(AstKind::StmtExpr), loc{0, 0, 0} {
        memset(&expr_stmt, 0, sizeof(expr_stmt));
    }
    ~Stmt() {}
};

// Function parameter
struct Param {
    StringView name;
    TypeExpr* type;
    ParamModifier modifier;
    SourceLocation loc;
};

// Variable declaration: var name: Type = init;
struct VarDecl {
    StringView name;
    TypeExpr* type;         // nullptr if type inference
    Expr* initializer;      // nullptr if no initializer
    bool is_pub;
    Type* resolved_type;    // Set by semantic analysis
};

// Function declaration: fun name(params): RetType { body }
struct FunDecl {
    StringView name;
    Span<Param> params;
    TypeExpr* return_type;  // nullptr if void
    Stmt* body;             // BlockStmt, nullptr if native
    bool is_pub;
    bool is_native;
};

// Struct field declaration
struct FieldDecl {
    StringView name;
    TypeExpr* type;
    Expr* default_value;  // nullptr if no default
    bool is_pub;
    SourceLocation loc;
};

// Struct declaration: struct Name : Parent { fields, methods }
struct StructDecl {
    StringView name;
    StringView parent_name;  // empty if no parent
    Span<FieldDecl> fields;
    Span<FunDecl*> methods;
    bool is_pub;
};

// Enum variant
struct EnumVariant {
    StringView name;
    Expr* value;  // nullptr if auto-assigned
    SourceLocation loc;
};

// Enum declaration: enum Name { variants }
struct EnumDecl {
    StringView name;
    Span<EnumVariant> variants;
    bool is_pub;
};

// Import name for selective imports
struct ImportName {
    StringView name;
    StringView alias;  // empty if no alias
    SourceLocation loc;
};

// Import declaration
// import pkg;
// from pkg import name1, name2;
// from pkg import name as alias;
struct ImportDecl {
    StringView module_path;
    Span<ImportName> names;  // empty for "import pkg;" style
    bool is_from_import;
};

// Declaration node
struct Decl {
    AstKind kind;
    SourceLocation loc;
    union {
        VarDecl var_decl;
        FunDecl fun_decl;
        StructDecl struct_decl;
        EnumDecl enum_decl;
        FieldDecl field_decl;
        ImportDecl import_decl;
        Stmt stmt;  // For statement declarations (like expression statements)
    };

    Decl() : kind(AstKind::DeclVar), loc{0, 0, 0} {
        memset(&var_decl, 0, sizeof(var_decl));
    }
    ~Decl() {}
};

// Program - the root AST node
struct Program {
    StringView module_name;       // Module name for visibility checking
    Span<Decl*> declarations;
};

}

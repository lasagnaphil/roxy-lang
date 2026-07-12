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
struct Type;

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
    ExprStructLiteral,
    ExprStringInterp,
    ExprLambda,

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
    StmtWhen,
    StmtThrow,
    StmtTry,
    StmtYield,

    // Declarations
    DeclVar,
    DeclFun,
    DeclStruct,
    DeclField,
    DeclEnum,
    DeclImport,
    DeclConstructor,
    DeclDestructor,
    DeclMethod,
    DeclTrait,
};

enum class LiteralKind : u8 {
    Nil,
    Bool,
    I32,    // Integer literal (default, no suffix)
    I64,    // Integer literal with 'l' suffix
    U32,    // Integer literal with 'u' suffix
    U64,    // Integer literal with 'ul' suffix
    F32,    // Float literal with 'f' suffix
    F64,    // Float literal (default, no suffix)
    String,
};

enum class UnaryOp : u8 {
    Negate,     // -
    Not,        // !
    BitNot,     // ~
    Ref,        // ref - borrow a reference
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
    BitXor,     // ^
    Shl,        // <<
    Shr,        // >>
};

enum class AssignOp : u8 {
    Assign,     // =
    AddAssign,  // +=
    SubAssign,  // -=
    MulAssign,  // *=
    DivAssign,  // /=
    ModAssign,  // %=
    BitAndAssign, // &=
    BitOrAssign,  // |=
    BitXorAssign, // ^=
    ShlAssign,    // <<=
    ShrAssign,    // >>=
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

// Discriminator for TypeExpr variants.
enum class TypeExprKind : u8 {
    Named,     // Identifier with optional generic args: Foo, Box<T>
    Function,  // Function type: fun(T1, T2) -> R or fun(T1) for void return
};

// Type parameter for generic declarations: <T, U> or <T: Trait1 + Trait2>
struct TypeParam {
    StringView name;
    SourceLocation loc;
    Span<TypeExpr*> bounds;  // Trait bounds (empty if unconstrained)
};

// Type expression for type annotations.
//   Named:    `name` is the type identifier; `type_args` are generic args.
//   Function: `name` is empty; `type_args` are parameter types; `return_type`
//             is the return type expression (nullptr for void).
struct TypeExpr {
    TypeExprKind kind = TypeExprKind::Named;
    StringView name;
    SourceLocation loc;
    RefKind ref_kind = RefKind::None;
    // `borrowed T`: a resolve-time transform applied on top of the resolved type
    //   borrowed (copyable T) -> T,  borrowed (uniq T) -> ref T,  ref/weak idempotent.
    // Composes with ref_kind (e.g. `borrowed uniq T` sets both). Never persists as
    // a Type — resolution maps it to a concrete type.
    bool is_borrowed = false;
    Span<TypeExpr*> type_args;
    TypeExpr* return_type = nullptr;  // Function only
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
    // Set when `name` resolves to a generic function template — analysis
    // can't pick the instantiation without surrounding context, so it marks
    // the expression and defers to coerce_generic_template_ref at the
    // assignment site (var init, arg passing, return, struct field).
    bool is_generic_template_ref;
    // Set after generic-template-ref coercion (or by the parser for explicit
    // `identity<i32>` syntax) to the monomorphized name (e.g. "identity$i32").
    // The IR builder routes through gen_function_ref with this name as the
    // call target.
    StringView mangled_name;
    // Explicit type arguments parsed in value position: `identity<i32>`. When
    // non-empty, semantic analysis instantiates the template with these types
    // directly (no inference from surrounding context).
    Span<TypeExpr*> generic_args;
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
// Also used for constructor calls: Point(1, 2) or uniq Point(1, 2)
struct CallExpr {
    Expr* callee;
    Span<CallArg> arguments;
    Span<TypeExpr*> type_args;    // Explicit type arguments: identity<i32>(42)
    // Named-constructor name — Point.from_coords(...) → "from_coords"; also
    // set by analysis when super.name(...) resolves to a parent's named
    // constructor (empty for default ctors and for method calls). See the
    // annotation contract above struct Expr.
    StringView constructor_name;
    // Set by semantic analysis: monomorphized name for generic calls, or the
    // native symbol for builtin method/constructor calls. See the annotation
    // contract above struct Expr.
    StringView mangled_name;
    bool is_heap;                 // true for "uniq Type(...)" constructor calls
};

// Index expression: arr[i]
struct IndexExpr {
    Expr* object;
    Expr* index;
};

// Get expression: obj.field
// After analysis, object->resolved_type == nullptr means `object` names an
// imported MODULE (module.member) — see the annotation contract above
// struct Expr.
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

// Field initializer for struct literals
struct FieldInit {
    StringView name;
    Expr* value;
    SourceLocation loc;
};

// Struct literal expression: Point { x = 1, y = 2 } or uniq Point { x = 1, y = 2 }
struct StructLiteralExpr {
    StringView type_name;
    Span<FieldInit> fields;
    Span<TypeExpr*> type_args;    // Generic type args: Box<i32> { value = 42 }
    StringView mangled_name;      // Set by semantic analysis for generic structs
    bool is_heap;                 // true for "uniq Type {...}", false for "Type {...}"
};

// String interpolation expression: f"text {expr} text"
struct StringInterpExpr {
    Span<StringView> parts;       // N+1 text segments (between expressions)
    Span<Expr*> expressions;      // N interpolated expressions
};

// How a captured variable enters the closure's environment.
enum class CaptureMode : u8 {
    Copy,   // implicit by-value capture (copyable type, ref/weak)
    Move,   // `[move name]` — ownership transferred from the outer scope
    Weak,   // `[weak self]` — capture as a weak ref (auto-wraps ref/uniq → weak)
};

// One entry in an explicit capture list: `[move name]`.
struct CaptureEntry {
    StringView name;
    CaptureMode mode;
    SourceLocation loc;
};

// Forward declaration for LambdaExpr.
struct Param;
struct Stmt;
struct Symbol;

// Resolved capture: produced by capture analysis and consumed by IR generation.
// One entry per distinct outer-scope variable referenced in the lambda body
// (or listed in the explicit `[move]` list, even if unreferenced).
struct CaptureInfo {
    StringView name;            // captured variable name (also the env field name)
    Type* type;                 // captured variable type (also the env field type)
    CaptureMode mode;           // Copy / Move / Weak
    Symbol* source_symbol;      // outer-scope symbol the capture refers to (null for self)
    SourceLocation loc;         // first reference site, for error attribution
    // Expression evaluated in the lambda's *enclosing* scope to obtain the
    // capture's value at construction time. For lambdas captured directly from
    // a containing function/block, this is `IdentifierExpr(name)`. For nested
    // closures where the variable lives further out, it's
    // `ExprGet(IdentifierExpr("__env"), name)` accessing the enclosing
    // lambda's env. For `self` captures, it's `ExprThis` (or a synthesized
    // struct literal for `[copy self]`).
    Expr* source_expr;
    // Set on implicit ref-self / [weak self] captures of a *copyable* struct,
    // where the receiver may be stack-allocated. The IR builder emits a
    // runtime slab-range check at the closure construction site that traps
    // with an actionable error if the check fails. Noncopyable structs are
    // provably heap-allocated and skip the check.
    bool needs_heap_check = false;
};

// Lambda expression:
//   fun(params): RetType { body }
//   fun(params): RetType => expr
//   fun[move x, move y](params): RetType { body }
struct LambdaExpr {
    Span<CaptureEntry> captures;  // Explicit capture list (empty for inferred-only)
    Span<Param> params;
    TypeExpr* return_type;        // nullptr means void
    Stmt* body;                   // Always a BlockStmt; `=> expr` lowers to `{ return expr; }`

    // Set by semantic analysis after synthesizing the env struct + lifted call function.
    // The env struct's first u32 field (`__call_idx`) holds the call function's index;
    // CALL_INDIRECT reads it at runtime to dispatch. Subsequent fields hold captured
    // values, in `resolved_captures` order.
    StringView env_struct_name;       // Name of the synthesized env struct ("__lambda_<id>_env")
    StringView call_function_name;    // Name of the synthesized call function ("__lambda_<id>_call")
    Type* env_struct_type;            // Resolved struct type pointer
    Span<CaptureInfo> resolved_captures;  // Captures discovered by analysis (env fields 1..N)
};

// Forward declaration
struct Type;

// ============================================================================
// The semantic→IR annotation contract
// ============================================================================
//
// Semantic analysis communicates its resolution results to the IR builder by
// annotating the AST in place. This comment is the authoritative spec of that
// contract: every writer lives in the semantic analyzer (or its
// collaborators), every reader in the IR builder. If you add an annotation or
// overload an existing one, document it here.
//
// --- Expr::resolved_type ---
// The expression's type, set by analyze_expr on every analyzed node (nullptr
// before analysis). Beyond that plain meaning, the analyzer overloads
// specific nodes' resolved_type as a dispatch side-channel:
//
//   CallExpr.callee->resolved_type (drives gen_call_expr dispatch):
//     primitive type  → primitive cast: i32(x), f64(y), bool(z)
//     struct type     → constructor call: Foo(...) / uniq Foo(...) (bare
//                       identifier callee; for named ctors Type.name(...) the
//                       GetExpr *object* carries the struct type instead)
//     List/Map type   → builtin container constructor; mangled_name holds the
//                       native ctor symbol ("List$$new" / "Map$$new")
//     ref<Ancestor>   → super call: the ancestor whose ctor/method the call
//                       resolved to (the mangling target). constructor_name
//                       distinguishes ctor from method — do NOT infer it from
//                       a void result type (super methods can return void).
//     function type   → ordinary / method / monomorphized-generic call. For
//                       method calls this is (ref<receiver>, params...) -> R
//                       built by build_method_function_type; the IR builder
//                       reads its param types for post-call move/nullify
//                       decisions on noncopyable arguments.
//
//   GetExpr.object->resolved_type == nullptr (after analysis) → the object
//     names an imported MODULE (module.member); the IR builder treats null as
//     "module-qualified access, no receiver". For real receivers the analyzer
//     re-sets the object's resolved_type to the receiver's original type,
//     reference wrapper included (uniq/ref/weak).
//
//   CatchClause.resolved_type == nullptr → catch-all clause (opaque
//     ExceptionRef); a typed catch carries the caught struct type.
//
// --- Name annotations (StringViews written by analysis, read at emit) ---
//   CallExpr.mangled_name         monomorphized name for generic calls
//                                 ("identity$i32"), or the native symbol for
//                                 builtin method/constructor calls
//                                 (list/map/coro/primitive methods).
//   CallExpr.constructor_name     named-constructor name for Type.name(...)
//                                 and super.name(...) constructor calls
//                                 (empty for default ctors and methods).
//   IdentifierExpr.mangled_name   monomorphized target for a generic function
//                                 reference in value position.
//   StructLiteralExpr.mangled_name / type_name
//                                 generic struct literals are rewritten to the
//                                 mangled instance name.
//
// --- Semantic-internal flags (never visible to the IR builder) ---
//   IdentifierExpr.is_generic_template_ref — set when a bare generic-function
//     name is referenced; resolved_type stays error_type until a coercion
//     site (var init, call arg, return, struct-literal field) binds the type
//     params via coerce_generic_template_ref, which clears the flag and
//     overwrites resolved_type. If no site coerces it, analysis reports an
//     error — the IR builder must never see the flag still set.
//
// --- AST mutation: the single-shot analysis rule ---
// Beyond annotating, analysis REWRITES the tree it walks:
//   - captured identifiers rewrite to ExprGet(__env, name); captured `self`
//     rewrites to ExprGet(__env, __self);
//   - generic TypeExprs rewrite to the mangled instance name with type_args
//     cleared;
//   - LambdaExpr gets env_struct_name / call_function_name / env_struct_type /
//     resolved_captures backfilled for closure emission (see LambdaExpr), and
//     lambda analysis synthesizes call-function decls and env struct types as
//     side effects.
// These rewrites are non-idempotent, so the rule is: an AST body is analyzed
// AT MOST ONCE (enforced by Decl::body_analyzed + assert at the body-analysis
// entry points). In-place mutation is deliberate — the annotations above are
// the IR builder's input, and side-band tables would tax every reader — so a
// consumer that needs to analyze the "same" code twice must analyze two
// trees:
//   - the LSP lowers a fresh AST from the CST for every analysis
//     (LspAnalysisContext::rebuild_declarations / analyze_function_body);
//   - generic templates are multi-consumer by design: every instantiation
//     deep-clones the pristine template (GenericInstantiator::clone_*), and
//     Phase B definition-site checking walks a throwaway
//     identity-substitution clone, never the template itself;
//   - trait default methods are cloned per implementing struct
//     (TraitSystem::inject_default_method).
// ============================================================================

// Expression node
struct Expr {
    AstKind kind;
    SourceLocation loc;
    Type* resolved_type;  // Set by semantic analysis; see the annotation contract above
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
        StructLiteralExpr struct_literal;
        StringInterpExpr string_interp;
        LambdaExpr lambda;
    };

    Expr() : kind(AstKind::ExprLiteral), loc{0, 0, 0, 0}, resolved_type(nullptr) {
        // Zero the entire union so any subsequently-activated variant starts
        // from a clean slate. Computing the size from the byte distance
        // between Expr and the union's first member keeps this correct as
        // variants grow (e.g. adding fields to IdentifierExpr).
        memset(&literal, 0, sizeof(*this) - (reinterpret_cast<char*>(&literal) - reinterpret_cast<char*>(this)));
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

// Delete statement: delete expr; or delete expr.dtor_name(args);
struct DeleteStmt {
    Expr* expr;
    StringView destructor_name;  // Empty for default destructor
    Span<CallArg> arguments;     // Destructor arguments (named destructors can have args)
};

// When case: case A, B: { body }
struct WhenCase {
    Span<StringView> case_names;  // "case A, B:" - can have multiple names
    Span<Decl*> body;             // Statements in the case body
    SourceLocation loc;
};

// When statement: when expr { case A: { ... } case B: { ... } else: { ... } }
struct WhenStmt {
    Expr* discriminant;           // The expression being matched
    Span<WhenCase> cases;         // List of cases
    Span<Decl*> else_body;        // Optional else body (empty if no else)
    SourceLocation else_loc;      // Location of else keyword
};

// Throw statement: throw expr;
struct ThrowStmt {
    Expr* expr;                   // Expression implementing Exception trait
};

// Catch clause: catch (e: Type) { ... } or catch (e) { ... }
struct CatchClause {
    StringView var_name;          // Catch variable name
    TypeExpr* exception_type;     // Type annotation (nullptr for catch-all)
    Stmt* body;                   // Block statement
    SourceLocation loc;
    // Set by semantic analysis: the caught struct type for a typed catch,
    // nullptr for a catch-all clause (opaque ExceptionRef) — see the
    // annotation contract above struct Expr.
    Type* resolved_type;
};

// Try statement: try { ... } catch (e: Type) { ... } finally { ... }
struct TryStmt {
    Stmt* try_body;               // Block statement
    Span<CatchClause> catches;
    Stmt* finally_body;           // nullptr if no finally
};

// Yield statement: yield expr;
struct YieldStmt {
    Expr* value;                  // Expression to yield
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
        WhenStmt when_stmt;
        ThrowStmt throw_stmt;
        TryStmt try_stmt;
        YieldStmt yield_stmt;
    };

    Stmt() : kind(AstKind::StmtExpr), loc{0, 0, 0, 0} {
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
    Type* resolved_type = nullptr;  // Set by semantic analysis
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
// or generic: fun name<T, U>(params): RetType { body }
struct FunDecl {
    StringView name;
    Span<TypeParam> type_params;  // Generic type params: <T, U>
    Span<Param> params;
    TypeExpr* return_type;  // nullptr if void
    Stmt* body;             // BlockStmt, nullptr if native
    bool is_pub;
    bool is_native;
    // Set by semantic analysis (register_fun_signature): true iff the return
    // type is Coro<T> AND the body contains a `yield`. A function that merely
    // returns/forwards a Coro<T> value (no yield) is NOT a coroutine — it is an
    // ordinary function producing a first-class coroutine value. Read by
    // analyze_fun_body and the IR builder to decide state-machine lowering.
    bool is_coroutine = false;
};

// Struct field declaration
struct FieldDecl {
    StringView name;
    TypeExpr* type;
    Expr* default_value;  // nullptr if no default
    bool is_pub;
    SourceLocation loc;
};

// Case in a when clause (field declarations for one variant)
struct WhenCaseFieldDecl {
    Span<StringView> case_names;  // "case A, B:" - can have multiple
    Span<FieldDecl> fields;       // Field declarations for this variant
    SourceLocation loc;
};

// When clause in struct definition (tagged union discriminant)
struct WhenFieldDecl {
    StringView discriminant_name;    // e.g., "type"
    TypeExpr* discriminant_type;     // e.g., SkillType (must be enum)
    Span<WhenCaseFieldDecl> cases;   // Variant cases
    SourceLocation loc;
};

// Struct declaration: struct Name : Parent { fields, when clauses, methods }
// or generic: struct Name<T, U> { ... }
struct StructDecl {
    StringView name;
    Span<TypeParam> type_params;    // Generic type params: <T, U>
    StringView parent_name;         // empty if no parent
    Span<FieldDecl> fields;         // Regular fields
    Span<WhenFieldDecl> when_clauses;  // Tagged union discriminants
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

// Constructor declaration: fun new StructName(params) { body }
// or: fun new StructName.ctor_name(params) { body }
// or generic: fun new StructName<T>(params) { body }
struct ConstructorDecl {
    StringView struct_name;
    StringView name;           // empty for default constructor
    Span<TypeParam> type_params;  // Struct type params: <T> in fun new Box<T>(...)
    Span<Param> params;
    Stmt* body;
    bool is_pub;
};

// Destructor declaration: fun delete StructName(params) { body }
// or: fun delete StructName.dtor_name(params) { body }
// or generic: fun delete StructName<T>() { body }
struct DestructorDecl {
    StringView struct_name;
    StringView name;           // empty for default destructor
    Span<TypeParam> type_params;  // Struct type params: <T> in fun delete Box<T>(...)
    Span<Param> params;        // Destructors CAN have parameters
    Stmt* body;
    bool is_pub;
};

// Method declaration: fun StructName.method_name(params): RetType { body }
// Also used for trait methods:  fun TraitName.method(params): RetType;    (body = nullptr)
// And trait implementations:    fun Type.method(params): RetType for Trait<Args> { body }
struct MethodDecl {
    StringView struct_name;
    StringView name;
    Span<TypeParam> type_params;   // Struct type params: <T> in fun List<T>.push(...)
    Span<Param> params;        // Does NOT include implicit self
    TypeExpr* return_type;     // nullptr if void
    Stmt* body;                // nullptr for required trait methods (no body)
    bool is_pub;
    bool is_native;            // true for native method declarations
    StringView trait_name;     // Non-empty for "fun Type.method() for Trait"
    Span<TypeExpr*> trait_type_args;   // Type args in "for Trait<Args>"
};

// Trait declaration: trait Name; or trait Name<T>; or trait Name : Parent;
struct TraitDecl {
    StringView name;
    Span<TypeParam> type_params;   // Generic type params: <T, U>
    StringView parent_name;    // empty if no parent trait
    bool is_pub;
};

// Declaration node
struct Decl {
    AstKind kind;
    SourceLocation loc;
    // Set by the body-analysis entry points. Guards the single-shot analysis
    // rule (see the annotation contract above struct Expr): analysis mutates
    // the tree it walks, so a body must never be analyzed twice — re-lower
    // (or clone) a fresh AST instead.
    bool body_analyzed;
    union {
        VarDecl var_decl;
        FunDecl fun_decl;
        StructDecl struct_decl;
        EnumDecl enum_decl;
        FieldDecl field_decl;
        ImportDecl import_decl;
        ConstructorDecl constructor_decl;
        DestructorDecl destructor_decl;
        MethodDecl method_decl;
        TraitDecl trait_decl;
        Stmt stmt;  // For statement declarations (like expression statements)
    };

    Decl() : kind(AstKind::DeclVar), loc{0, 0, 0, 0}, body_analyzed(false) {
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

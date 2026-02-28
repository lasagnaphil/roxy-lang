#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/span.hpp"
#include "roxy/core/string.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/shared/token.hpp"

namespace rx {

// Byte offset range in source text
struct TextRange {
    u32 start;
    u32 end;
};

// SyntaxKind identifies each node in the CST.
// Terminal kinds map 1:1 from TokenKind; non-terminal kinds cover grammar productions.
enum class SyntaxKind : u16 {
    // ---- Terminal kinds (one per TokenKind) ----
    TokenIntLiteral,
    TokenFloatLiteral,
    TokenStringLiteral,
    TokenFStringBegin,
    TokenFStringMid,
    TokenFStringEnd,
    TokenIdentifier,

    TokenLeftParen,
    TokenRightParen,
    TokenLeftBrace,
    TokenRightBrace,
    TokenLeftBracket,
    TokenRightBracket,
    TokenComma,
    TokenDot,
    TokenSemicolon,
    TokenColon,
    TokenQuestion,
    TokenTilde,

    TokenPlus,
    TokenPlusEqual,
    TokenMinus,
    TokenMinusEqual,
    TokenStar,
    TokenStarEqual,
    TokenSlash,
    TokenSlashEqual,
    TokenPercent,
    TokenPercentEqual,
    TokenBang,
    TokenBangEqual,
    TokenEqual,
    TokenEqualEqual,
    TokenLess,
    TokenLessEqual,
    TokenGreater,
    TokenGreaterEqual,
    TokenAmp,
    TokenAmpAmp,
    TokenAmpEqual,
    TokenPipe,
    TokenPipePipe,
    TokenPipeEqual,
    TokenCaret,
    TokenCaretEqual,
    TokenLessLess,
    TokenLessLessEqual,
    TokenGreaterGreater,
    TokenGreaterGreaterEqual,
    TokenColonColon,

    TokenKwTrue,
    TokenKwFalse,
    TokenKwNil,
    TokenKwVar,
    TokenKwFun,
    TokenKwStruct,
    TokenKwEnum,
    TokenKwTrait,
    TokenKwPub,
    TokenKwNative,

    TokenKwIf,
    TokenKwElse,
    TokenKwFor,
    TokenKwWhile,
    TokenKwBreak,
    TokenKwContinue,
    TokenKwReturn,
    TokenKwWhen,
    TokenKwCase,

    TokenKwTry,
    TokenKwCatch,
    TokenKwThrow,
    TokenKwFinally,

    TokenKwYield,

    TokenKwSelf,
    TokenKwSuper,
    TokenKwNew,
    TokenKwDelete,

    TokenKwUniq,
    TokenKwRef,
    TokenKwWeak,
    TokenKwOut,
    TokenKwInout,

    TokenKwImport,
    TokenKwFrom,

    TokenError,
    TokenEof,

    // ---- Non-terminal kinds (grammar productions) ----

    // Top-level
    NodeProgram,

    // Declarations
    NodeVarDecl,
    NodeFunDecl,
    NodeMethodDecl,
    NodeConstructorDecl,
    NodeDestructorDecl,
    NodeStructDecl,
    NodeEnumDecl,
    NodeTraitDecl,
    NodeImportDecl,
    NodeFieldDecl,

    // Sub-nodes for declarations
    NodeParam,
    NodeParamList,
    NodeTypeParam,
    NodeTypeParamList,
    NodeTypeArg,
    NodeTypeArgList,
    NodeTypeExpr,
    NodeEnumVariant,
    NodeImportName,
    NodeWhenFieldDecl,
    NodeWhenCaseFieldDecl,
    NodeFieldInit,
    NodeTraitBounds,

    // Statements
    NodeExprStmt,
    NodeBlockStmt,
    NodeIfStmt,
    NodeWhileStmt,
    NodeForStmt,
    NodeReturnStmt,
    NodeBreakStmt,
    NodeContinueStmt,
    NodeDeleteStmt,
    NodeWhenStmt,
    NodeWhenCase,
    NodeThrowStmt,
    NodeTryStmt,
    NodeCatchClause,

    // Expressions
    NodeLiteralExpr,
    NodeIdentifierExpr,
    NodeUnaryExpr,
    NodeBinaryExpr,
    NodeTernaryExpr,
    NodeCallExpr,
    NodeIndexExpr,
    NodeGetExpr,
    NodeStaticGetExpr,
    NodeAssignExpr,
    NodeGroupingExpr,
    NodeSelfExpr,
    NodeSuperExpr,
    NodeStructLiteralExpr,
    NodeStringInterpExpr,
    NodeUniqExpr,
    NodeCallArgList,
    NodeCallArg,

    // Error recovery marker
    Error,
};

// Convert a TokenKind to the corresponding terminal SyntaxKind
inline SyntaxKind token_kind_to_syntax_kind(TokenKind kind) {
    // Terminal SyntaxKind values are laid out in the same order as TokenKind
    return static_cast<SyntaxKind>(static_cast<u16>(kind));
}

// A single node in the concrete syntax tree
struct SyntaxNode {
    SyntaxKind kind;
    TextRange range;
    SyntaxNode* parent;
    Span<SyntaxNode*> children;  // finalized from Vector via alloc_span
    Token token;                 // set for leaf (terminal) nodes
    const char* error_message;   // set for Error nodes
};

// A diagnostic collected during parsing
struct ParseDiagnostic {
    TextRange range;
    String message;
};

// The result of parsing: a CST plus diagnostics
struct SyntaxTree {
    SyntaxNode* root;
    Vector<ParseDiagnostic> diagnostics;
    const char* source;
    u32 source_length;
};

} // namespace rx

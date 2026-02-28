#pragma once

namespace rx {

enum class TokenKind {
    // Literals
    IntLiteral,
    FloatLiteral,
    StringLiteral,
    FStringBegin,   // f"text{      — text before first interpolation
    FStringMid,     // }text{       — text between interpolations
    FStringEnd,     // }text"       — text after last interpolation
    Identifier,

    // Single-character tokens
    LeftParen,      // (
    RightParen,     // )
    LeftBrace,      // {
    RightBrace,     // }
    LeftBracket,    // [
    RightBracket,   // ]
    Comma,          // ,
    Dot,            // .
    Semicolon,      // ;
    Colon,          // :
    Question,       // ?
    Tilde,          // ~

    // One or two character tokens
    Plus,           // +
    PlusEqual,      // +=
    Minus,          // -
    MinusEqual,     // -=
    Star,           // *
    StarEqual,      // *=
    Slash,          // /
    SlashEqual,     // /=
    Percent,        // %
    PercentEqual,   // %=
    Bang,           // !
    BangEqual,      // !=
    Equal,          // =
    EqualEqual,     // ==
    Less,           // <
    LessEqual,      // <=
    Greater,        // >
    GreaterEqual,   // >=
    Amp,            // &
    AmpAmp,         // &&
    AmpEqual,       // &=
    Pipe,           // |
    PipePipe,       // ||
    PipeEqual,      // |=
    Caret,          // ^
    CaretEqual,     // ^=
    LessLess,       // <<
    LessLessEqual,  // <<=
    GreaterGreater, // >>
    GreaterGreaterEqual, // >>=
    ColonColon,     // ::

    // Keywords - Types/modifiers
    KwTrue,
    KwFalse,
    KwNil,
    KwVar,
    KwFun,
    KwStruct,
    KwEnum,
    KwTrait,
    KwPub,
    KwNative,

    // Keywords - Control flow
    KwIf,
    KwElse,
    KwFor,
    KwWhile,
    KwBreak,
    KwContinue,
    KwReturn,
    KwWhen,
    KwCase,

    // Keywords - Exception handling
    KwTry,
    KwCatch,
    KwThrow,
    KwFinally,

    // Keywords - OOP
    KwSelf,
    KwSuper,
    KwNew,
    KwDelete,

    // Keywords - References
    KwUniq,
    KwRef,
    KwWeak,
    KwOut,
    KwInout,

    // Keywords - Imports
    KwImport,
    KwFrom,

    // Special tokens
    Error,
    Eof,
};

const char* token_kind_to_string(TokenKind kind);

}

#pragma once

namespace rx {

enum class TokenKind {
    // Literals
    IntLiteral,
    FloatLiteral,
    StringLiteral,
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
    Pipe,           // |
    PipePipe,       // ||
    ColonColon,     // ::

    // Keywords - Types/modifiers
    KwTrue,
    KwFalse,
    KwNil,
    KwVar,
    KwFun,
    KwStruct,
    KwEnum,
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

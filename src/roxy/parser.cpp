#include "roxy/parser.hpp"

namespace rx {

ParseRule Parser::s_parse_rules[] = {
        [(u32) TokenType::LeftParen]     = {&Parser::grouping,   &Parser::call,          Precedence::Call},
        [(u32) TokenType::RightParen]    = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::LeftBrace]     = {&Parser::table,      NULL,                   Precedence::None},
        [(u32) TokenType::RightBrace]    = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::LeftBrace]     = {&Parser::array,      &Parser::subscript,     Precedence::Call},
        [(u32) TokenType::RightBrace]    = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Comma]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Dot]           = {NULL,                &Parser::dot,           Precedence::Call},
        [(u32) TokenType::Minus]         = {&Parser::unary,      &Parser::binary,        Precedence::Term},
        [(u32) TokenType::Plus]          = {NULL,                &Parser::binary,        Precedence::Term},
        [(u32) TokenType::Semicolon]     = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Slash]         = {NULL,                &Parser::binary,        Precedence::Factor},
        [(u32) TokenType::Star]          = {NULL,                &Parser::binary,        Precedence::Factor},
        [(u32) TokenType::Percent]       = {NULL,                &Parser::binary,        Precedence::Term},
        [(u32) TokenType::QuestionMark]  = {NULL,                &Parser::ternary,       Precedence::Ternary},
        [(u32) TokenType::Colon]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Ampersand]     = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Bar]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Tilde]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Caret]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Bang]          = {&Parser::unary,      NULL,                   Precedence::None},
        [(u32) TokenType::BangEqual]     = {NULL,                &Parser::binary,        Precedence::Equality},
        [(u32) TokenType::Equal]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::EqualEqual]    = {NULL,                &Parser::binary,        Precedence::Equality},
        [(u32) TokenType::Greater]       = {NULL,                &Parser::binary,        Precedence::Comparison},
        [(u32) TokenType::GreaterEqual]  = {NULL,                &Parser::binary,        Precedence::Comparison},
        [(u32) TokenType::Less]          = {NULL,                &Parser::binary,        Precedence::Comparison},
        [(u32) TokenType::LessEqual]     = {NULL,                &Parser::binary,        Precedence::Comparison},
        [(u32) TokenType::AmpAmp]        = {NULL,                &Parser::logical_and,   Precedence::And},
        [(u32) TokenType::BarBar]        = {NULL,                &Parser::logical_or,    Precedence::Or},
        [(u32) TokenType::Identifier]    = {&Parser::variable,   NULL,                   Precedence::None},
        [(u32) TokenType::String]        = {&Parser::string,     NULL,                   Precedence::None},
        [(u32) TokenType::NumberInt]     = {&Parser::number_i,   NULL,                   Precedence::None},
        [(u32) TokenType::NumberFloat]   = {&Parser::number_f,   NULL,                   Precedence::None},
        [(u32) TokenType::Struct]        = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Else]          = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::False]         = {&Parser::literal,    NULL,                   Precedence::None},
        [(u32) TokenType::For]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Fun]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::If]            = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Nil]           = {&Parser::literal,    NULL,                   Precedence::None},
        [(u32) TokenType::Native]        = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Return]        = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Super]         = {&Parser::super,      NULL,                   Precedence::None},
        [(u32) TokenType::This]          = {&Parser::this_,      NULL,                   Precedence::None},
        [(u32) TokenType::True]          = {&Parser::literal,    NULL,                   Precedence::None},
        [(u32) TokenType::Var]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::While]         = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::Eof]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::ErrorUnterminatedString]           = {NULL,                NULL,                   Precedence::None},
        [(u32) TokenType::ErrorUnexpectedCharacter]           = {NULL,                NULL,                   Precedence::None},
};

}
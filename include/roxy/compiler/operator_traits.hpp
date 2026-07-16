#pragma once

#include "roxy/compiler/ast.hpp"

#include <cstring>

namespace rx {

// Maps binary operators to trait method names. Returns an empty view for
// operators without a trait mapping (e.g., And/Or). The view carries a
// compile-time length (via _sv), so callers avoid a per-call strlen.
inline StringView binary_op_to_trait_method(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:      return "add"_sv;
        case BinaryOp::Sub:      return "sub"_sv;
        case BinaryOp::Mul:      return "mul"_sv;
        case BinaryOp::Div:      return "div"_sv;
        case BinaryOp::Mod:      return "mod"_sv;
        case BinaryOp::BitAnd:   return "bit_and"_sv;
        case BinaryOp::BitOr:    return "bit_or"_sv;
        case BinaryOp::BitXor:   return "bit_xor"_sv;
        case BinaryOp::Shl:      return "shl"_sv;
        case BinaryOp::Shr:      return "shr"_sv;
        case BinaryOp::Equal:    return "eq"_sv;
        case BinaryOp::NotEqual: return "ne"_sv;
        case BinaryOp::Less:     return "lt"_sv;
        case BinaryOp::LessEq:   return "le"_sv;
        case BinaryOp::Greater:  return "gt"_sv;
        case BinaryOp::GreaterEq: return "ge"_sv;
        default: return {};
    }
}

// Maps unary operators to trait method names. Empty view = no mapping.
inline StringView unary_op_to_trait_method(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate: return "neg"_sv;
        case UnaryOp::BitNot: return "bit_not"_sv;
        default: return {};
    }
}

// Maps binary operators to their source spelling (for diagnostics).
inline const char* binary_op_to_symbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:      return "+";
        case BinaryOp::Sub:      return "-";
        case BinaryOp::Mul:      return "*";
        case BinaryOp::Div:      return "/";
        case BinaryOp::Mod:      return "%";
        case BinaryOp::BitAnd:   return "&";
        case BinaryOp::BitOr:    return "|";
        case BinaryOp::BitXor:   return "^";
        case BinaryOp::Shl:      return "<<";
        case BinaryOp::Shr:      return ">>";
        case BinaryOp::Equal:    return "==";
        case BinaryOp::NotEqual: return "!=";
        case BinaryOp::Less:     return "<";
        case BinaryOp::LessEq:   return "<=";
        case BinaryOp::Greater:  return ">";
        case BinaryOp::GreaterEq: return ">=";
        case BinaryOp::And:      return "&&";
        case BinaryOp::Or:       return "||";
        default: return "?";
    }
}

// Maps unary operators to their source spelling (for diagnostics).
inline const char* unary_op_to_symbol(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate: return "-";
        case UnaryOp::Not:    return "!";
        case UnaryOp::BitNot: return "~";
        default: return "?";
    }
}

// Maps compound assignment operators to trait method names.
inline const char* assign_op_to_trait_method(AssignOp op) {
    switch (op) {
        case AssignOp::AddAssign:    return "add_assign";
        case AssignOp::SubAssign:    return "sub_assign";
        case AssignOp::MulAssign:    return "mul_assign";
        case AssignOp::DivAssign:    return "div_assign";
        case AssignOp::ModAssign:    return "mod_assign";
        case AssignOp::BitAndAssign: return "bit_and_assign";
        case AssignOp::BitOrAssign:  return "bit_or_assign";
        case AssignOp::BitXorAssign: return "bit_xor_assign";
        case AssignOp::ShlAssign:    return "shl_assign";
        case AssignOp::ShrAssign:    return "shr_assign";
        default: return nullptr;
    }
}

}

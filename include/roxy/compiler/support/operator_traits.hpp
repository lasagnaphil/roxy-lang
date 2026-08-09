#pragma once

#include "roxy/compiler/parse/ast.hpp"

#include <cstring>

namespace rx {

// Maps binary operators to trait method names. Returns an empty view for
// operators without a trait mapping (e.g., And/Or). The view carries a
// compile-time length (via _sv), so callers avoid a per-call strlen.
inline StringView binary_op_to_trait_method(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:
            return "add"_sv;
        case BinaryOp::Sub:
            return "sub"_sv;
        case BinaryOp::Mul:
            return "mul"_sv;
        case BinaryOp::Div:
            return "div"_sv;
        case BinaryOp::Mod:
            return "mod"_sv;
        case BinaryOp::BitAnd:
            return "bit_and"_sv;
        case BinaryOp::BitOr:
            return "bit_or"_sv;
        case BinaryOp::BitXor:
            return "bit_xor"_sv;
        case BinaryOp::Shl:
            return "shl"_sv;
        case BinaryOp::Shr:
            return "shr"_sv;
        case BinaryOp::Equal:
            return "eq"_sv;
        case BinaryOp::NotEqual:
            return "ne"_sv;
        case BinaryOp::Less:
            return "lt"_sv;
        case BinaryOp::LessEq:
            return "le"_sv;
        case BinaryOp::Greater:
            return "gt"_sv;
        case BinaryOp::GreaterEq:
            return "ge"_sv;
        default:
            return {};
    }
}

// Maps unary operators to trait method names. Empty view = no mapping.
inline StringView unary_op_to_trait_method(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate:
            return "neg"_sv;
        case UnaryOp::BitNot:
            return "bit_not"_sv;
        default:
            return {};
    }
}

// Inverse of binary_op_to_trait_method. Returns false for names with no
// operator (including every *_assign method — compound assigns need an lvalue
// receiver, so an explicit method call like `a.add_assign(b)` on a by-value
// primitive receiver is deliberately unlowerable). Kept adjacent to the forward
// map so the two stay a single source of truth.
inline bool trait_method_to_binary_op(StringView name, BinaryOp& out) {
    if (name == "add"_sv) {
        out = BinaryOp::Add;
        return true;
    }
    if (name == "sub"_sv) {
        out = BinaryOp::Sub;
        return true;
    }
    if (name == "mul"_sv) {
        out = BinaryOp::Mul;
        return true;
    }
    if (name == "div"_sv) {
        out = BinaryOp::Div;
        return true;
    }
    if (name == "mod"_sv) {
        out = BinaryOp::Mod;
        return true;
    }
    if (name == "bit_and"_sv) {
        out = BinaryOp::BitAnd;
        return true;
    }
    if (name == "bit_or"_sv) {
        out = BinaryOp::BitOr;
        return true;
    }
    if (name == "bit_xor"_sv) {
        out = BinaryOp::BitXor;
        return true;
    }
    if (name == "shl"_sv) {
        out = BinaryOp::Shl;
        return true;
    }
    if (name == "shr"_sv) {
        out = BinaryOp::Shr;
        return true;
    }
    if (name == "eq"_sv) {
        out = BinaryOp::Equal;
        return true;
    }
    if (name == "ne"_sv) {
        out = BinaryOp::NotEqual;
        return true;
    }
    if (name == "lt"_sv) {
        out = BinaryOp::Less;
        return true;
    }
    if (name == "le"_sv) {
        out = BinaryOp::LessEq;
        return true;
    }
    if (name == "gt"_sv) {
        out = BinaryOp::Greater;
        return true;
    }
    if (name == "ge"_sv) {
        out = BinaryOp::GreaterEq;
        return true;
    }
    return false;
}

// Inverse of unary_op_to_trait_method.
inline bool trait_method_to_unary_op(StringView name, UnaryOp& out) {
    if (name == "neg"_sv) {
        out = UnaryOp::Negate;
        return true;
    }
    if (name == "bit_not"_sv) {
        out = UnaryOp::BitNot;
        return true;
    }
    return false;
}

// True for the comparison operators (Equal..GreaterEq — contiguous in the
// BinaryOp declaration), which lower through get_comparison_op rather than
// get_binary_op.
inline bool is_comparison_binary_op(BinaryOp op) {
    return op >= BinaryOp::Equal && op <= BinaryOp::GreaterEq;
}

// Maps binary operators to their source spelling (for diagnostics).
inline const char* binary_op_to_symbol(BinaryOp op) {
    switch (op) {
        case BinaryOp::Add:
            return "+";
        case BinaryOp::Sub:
            return "-";
        case BinaryOp::Mul:
            return "*";
        case BinaryOp::Div:
            return "/";
        case BinaryOp::Mod:
            return "%";
        case BinaryOp::BitAnd:
            return "&";
        case BinaryOp::BitOr:
            return "|";
        case BinaryOp::BitXor:
            return "^";
        case BinaryOp::Shl:
            return "<<";
        case BinaryOp::Shr:
            return ">>";
        case BinaryOp::Equal:
            return "==";
        case BinaryOp::NotEqual:
            return "!=";
        case BinaryOp::Less:
            return "<";
        case BinaryOp::LessEq:
            return "<=";
        case BinaryOp::Greater:
            return ">";
        case BinaryOp::GreaterEq:
            return ">=";
        case BinaryOp::And:
            return "&&";
        case BinaryOp::Or:
            return "||";
        default:
            return "?";
    }
}

// Maps unary operators to their source spelling (for diagnostics).
inline const char* unary_op_to_symbol(UnaryOp op) {
    switch (op) {
        case UnaryOp::Negate:
            return "-";
        case UnaryOp::Not:
            return "!";
        case UnaryOp::BitNot:
            return "~";
        default:
            return "?";
    }
}

// Maps compound assignment operators to trait method names.
inline const char* assign_op_to_trait_method(AssignOp op) {
    switch (op) {
        case AssignOp::AddAssign:
            return "add_assign";
        case AssignOp::SubAssign:
            return "sub_assign";
        case AssignOp::MulAssign:
            return "mul_assign";
        case AssignOp::DivAssign:
            return "div_assign";
        case AssignOp::ModAssign:
            return "mod_assign";
        case AssignOp::BitAndAssign:
            return "bit_and_assign";
        case AssignOp::BitOrAssign:
            return "bit_or_assign";
        case AssignOp::BitXorAssign:
            return "bit_xor_assign";
        case AssignOp::ShlAssign:
            return "shl_assign";
        case AssignOp::ShrAssign:
            return "shr_assign";
        default:
            return nullptr;
    }
}

} // namespace rx

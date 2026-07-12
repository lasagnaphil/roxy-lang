#include "roxy/compiler/ir_fold.hpp"

#include <climits>

namespace rx {

bool fold_binary_const(IROp op, const IRInst* left, const IRInst* right, FoldedConst& out) {
    if (!left || !right) return false;

    switch (op) {
    case IROp::AddI:
    case IROp::SubI:
    case IROp::MulI:
    case IROp::DivI:
    case IROp::ModI: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        i64 a = left->const_data.int_val;
        i64 b = right->const_data.int_val;
        u64 ua = static_cast<u64>(a);
        u64 ub = static_cast<u64>(b);
        i64 result;
        switch (op) {
            case IROp::AddI: result = static_cast<i64>(ua + ub); break;
            case IROp::SubI: result = static_cast<i64>(ua - ub); break;
            case IROp::MulI: result = static_cast<i64>(ua * ub); break;
            case IROp::DivI:
                if (b == 0 || (a == INT64_MIN && b == -1)) return false;
                result = a / b;
                break;
            case IROp::ModI:
                if (b == 0 || (a == INT64_MIN && b == -1)) return false;
                result = a % b;
                break;
            default: return false;
        }
        out.kind = FoldedConst::Kind::Int;
        out.int_val = result;
        return true;
    }

    case IROp::EqI:
    case IROp::NeI:
    case IROp::LtI:
    case IROp::LeI:
    case IROp::GtI:
    case IROp::GeI: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        i64 a = left->const_data.int_val;
        i64 b = right->const_data.int_val;
        bool result;
        switch (op) {
            case IROp::EqI: result = a == b; break;
            case IROp::NeI: result = a != b; break;
            case IROp::LtI: result = a < b; break;
            case IROp::LeI: result = a <= b; break;
            case IROp::GtI: result = a > b; break;
            case IROp::GeI: result = a >= b; break;
            default: return false;
        }
        out.kind = FoldedConst::Kind::Bool;
        out.bool_val = result;
        return true;
    }

    case IROp::BitAnd:
    case IROp::BitOr:
    case IROp::BitXor: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        u64 a = static_cast<u64>(left->const_data.int_val);
        u64 b = static_cast<u64>(right->const_data.int_val);
        u64 result = (op == IROp::BitAnd) ? (a & b)
                   : (op == IROp::BitOr)  ? (a | b)
                                          : (a ^ b);
        out.kind = FoldedConst::Kind::Int;
        out.int_val = static_cast<i64>(result);
        return true;
    }

    case IROp::Shl:
    case IROp::Shr: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        u64 a = static_cast<u64>(left->const_data.int_val);
        u64 b = static_cast<u64>(right->const_data.int_val) & 63;
        out.kind = FoldedConst::Kind::Int;
        out.int_val = (op == IROp::Shl) ? static_cast<i64>(a << b)
                                        : (left->const_data.int_val >> b);  // arithmetic shift on signed
        return true;
    }

    // f32 / f64 arithmetic — IEEE-754 host assumed.
    case IROp::AddF:
    case IROp::SubF:
    case IROp::MulF:
    case IROp::DivF: {
        if (left->op != IROp::ConstF || right->op != IROp::ConstF) return false;
        f32 a = left->const_data.f32_val;
        f32 b = right->const_data.f32_val;
        f32 result = (op == IROp::AddF) ? (a + b)
                   : (op == IROp::SubF) ? (a - b)
                   : (op == IROp::MulF) ? (a * b)
                                        : (a / b);
        out.kind = FoldedConst::Kind::Float;
        out.float_val = static_cast<f64>(result);
        return true;
    }

    case IROp::AddD:
    case IROp::SubD:
    case IROp::MulD:
    case IROp::DivD: {
        if (left->op != IROp::ConstD || right->op != IROp::ConstD) return false;
        f64 a = left->const_data.f64_val;
        f64 b = right->const_data.f64_val;
        out.kind = FoldedConst::Kind::Float;
        out.float_val = (op == IROp::AddD) ? (a + b)
                      : (op == IROp::SubD) ? (a - b)
                      : (op == IROp::MulD) ? (a * b)
                                           : (a / b);
        return true;
    }

    case IROp::And:
    case IROp::Or: {
        if (left->op != IROp::ConstBool || right->op != IROp::ConstBool) return false;
        bool a = left->const_data.bool_val;
        bool b = right->const_data.bool_val;
        out.kind = FoldedConst::Kind::Bool;
        out.bool_val = (op == IROp::And) ? (a && b) : (a || b);
        return true;
    }

    // Unsigned integer division/modulo — unsigned semantics; only div-by-zero
    // is left unfolded (no INT_MIN/-1 overflow case for unsigned).
    case IROp::DivU:
    case IROp::ModU: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        u64 a = static_cast<u64>(left->const_data.int_val);
        u64 b = static_cast<u64>(right->const_data.int_val);
        if (b == 0) return false;
        out.kind = FoldedConst::Kind::Int;
        out.int_val = static_cast<i64>((op == IROp::DivU) ? (a / b) : (a % b));
        return true;
    }

    // Unsigned ordered comparisons.
    case IROp::LtU:
    case IROp::LeU:
    case IROp::GtU:
    case IROp::GeU: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        u64 a = static_cast<u64>(left->const_data.int_val);
        u64 b = static_cast<u64>(right->const_data.int_val);
        out.kind = FoldedConst::Kind::Bool;
        out.bool_val = (op == IROp::LtU) ? (a < b)
                     : (op == IROp::LeU) ? (a <= b)
                     : (op == IROp::GtU) ? (a > b)
                                         : (a >= b);
        return true;
    }

    // Logical (unsigned) shift right.
    case IROp::UShr: {
        if (left->op != IROp::ConstInt || right->op != IROp::ConstInt) return false;
        u64 a = static_cast<u64>(left->const_data.int_val);
        u64 b = static_cast<u64>(right->const_data.int_val) & 63;
        out.kind = FoldedConst::Kind::Int;
        out.int_val = static_cast<i64>(a >> b);
        return true;
    }

    default:
        return false;
    }
}

bool fold_unary_const(IROp op, const IRInst* operand, FoldedConst& out) {
    if (!operand) return false;

    switch (op) {
    case IROp::NegI: {
        if (operand->op != IROp::ConstInt) return false;
        u64 v = static_cast<u64>(operand->const_data.int_val);
        out.kind = FoldedConst::Kind::Int;
        out.int_val = static_cast<i64>(0 - v);
        return true;
    }
    case IROp::NegF: {
        if (operand->op != IROp::ConstF) return false;
        out.kind = FoldedConst::Kind::Float;
        out.float_val = static_cast<f64>(-operand->const_data.f32_val);
        return true;
    }
    case IROp::NegD: {
        if (operand->op != IROp::ConstD) return false;
        out.kind = FoldedConst::Kind::Float;
        out.float_val = -operand->const_data.f64_val;
        return true;
    }
    case IROp::Not: {
        if (operand->op != IROp::ConstBool) return false;
        out.kind = FoldedConst::Kind::Bool;
        out.bool_val = !operand->const_data.bool_val;
        return true;
    }
    case IROp::BitNot: {
        if (operand->op != IROp::ConstInt) return false;
        u64 v = static_cast<u64>(operand->const_data.int_val);
        out.kind = FoldedConst::Kind::Int;
        out.int_val = static_cast<i64>(~v);
        return true;
    }
    default:
        return false;
    }
}

// Narrow an i64 value to `bits` bits, sign- or zero-extending as appropriate.
// Mirrors the runtime's TRUNC_S / TRUNC_U bytecode behavior.
static i64 narrow_int_to_width(i64 v, u8 bits, bool is_signed) {
    switch (bits) {
        case 8:  return is_signed ? static_cast<i64>(static_cast<i8>(v))  : static_cast<i64>(static_cast<u8>(v));
        case 16: return is_signed ? static_cast<i64>(static_cast<i16>(v)) : static_cast<i64>(static_cast<u16>(v));
        case 32: return is_signed ? static_cast<i64>(static_cast<i32>(v)) : static_cast<i64>(static_cast<u32>(v));
        default: return v;
    }
}

static u8 type_int_bits(TypeKind k) {
    switch (k) {
        case TypeKind::I8:  case TypeKind::U8:  return 8;
        case TypeKind::I16: case TypeKind::U16: return 16;
        case TypeKind::I32: case TypeKind::U32: return 32;
        case TypeKind::I64: case TypeKind::U64: return 64;
        default: return 64;
    }
}

bool fold_cast_const(const IRInst* source, const Type* source_type, const Type* target_type,
                     FoldedConst& out) {
    if (!source || !source_type || !target_type) return false;

    bool src_int  = (source->op == IROp::ConstInt);
    bool src_bool = (source->op == IROp::ConstBool);
    bool src_f32  = (source->op == IROp::ConstF);
    bool src_f64  = (source->op == IROp::ConstD);
    if (!src_int && !src_bool && !src_f32 && !src_f64) return false;

    // Target = bool: nonzero/true
    if (target_type->kind == TypeKind::Bool) {
        out.kind = FoldedConst::Kind::Bool;
        out.bool_val = src_int  ? (source->const_data.int_val != 0)
                     : src_bool ? source->const_data.bool_val
                     : src_f32  ? (source->const_data.f32_val != 0.0f)
                                : (source->const_data.f64_val != 0.0);
        return true;
    }

    // Target = integer
    if (target_type->is_integer()) {
        i64 v = src_int  ? source->const_data.int_val
              : src_bool ? (source->const_data.bool_val ? 1 : 0)
              : src_f32  ? static_cast<i64>(source->const_data.f32_val)
                         : static_cast<i64>(source->const_data.f64_val);
        out.kind = FoldedConst::Kind::Int;
        out.int_val = narrow_int_to_width(v, type_int_bits(target_type->kind),
                                          target_type->is_signed_integer());
        return true;
    }

    // Target = f32 / f64
    if (target_type->is_float()) {
        f64 v;
        if (src_int) {
            v = source_type->is_unsigned_integer()
                ? static_cast<f64>(static_cast<u64>(source->const_data.int_val))
                : static_cast<f64>(source->const_data.int_val);
        } else if (src_bool) {
            v = source->const_data.bool_val ? 1.0 : 0.0;
        } else if (src_f32) {
            v = static_cast<f64>(source->const_data.f32_val);
        } else {
            v = source->const_data.f64_val;
        }
        out.kind = FoldedConst::Kind::Float;
        out.float_val = v;
        return true;
    }

    return false;
}

}

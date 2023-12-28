#pragma once

#include "roxy/core/types.hpp"
#include "roxy/type.hpp"

// IL Opcode list for the Roxy language.

#define OPCODE_LIST(X)      \
    X(nop)                  \
    X(brk)                \
    X(iload_0)             \
    X(iload_1)             \
    X(iload_2)             \
    X(iload_3)             \
    X(istore_0)             \
    X(istore_1)             \
    X(istore_2)             \
    X(istore_3)             \
    X(iload)              \
    X(iload_s)             \
    X(istore)             \
    X(istore_s)             \
    X(lload_0)          \
    X(lload_1)          \
    X(lload_2)          \
    X(lload_3)          \
    X(lstore_0)          \
    X(lstore_1)          \
    X(lstore_2)          \
    X(lstore_3)          \
    X(lload)            \
    X(lload_s)          \
    X(lstore)            \
    X(lstore_s)             \
    X(rload_0)          \
    X(rload_1)          \
    X(rload_2)          \
    X(rload_3)          \
    X(rstore_0)          \
    X(rstore_1)          \
    X(rstore_2)          \
    X(rstore_3)          \
    X(rload)            \
    X(rload_s)          \
    X(rstore)            \
    X(rstore_s)             \
    X(iconst_nil)             \
    X(iconst_m1)            \
    X(iconst_0)             \
    X(iconst_1)             \
    X(iconst_2)             \
    X(iconst_3)             \
    X(iconst_4)             \
    X(iconst_5)             \
    X(iconst_6)             \
    X(iconst_7)             \
    X(iconst_8)             \
    X(iconst_s)             \
    X(iconst)                \
    X(lconst)                \
    X(fconst)                \
    X(dconst)                \
    X(dup)                  \
    X(pop)                  \
    X(call)                 \
    X(callnative)                 \
    X(ret)                  \
    X(iret)                  \
    X(lret)                  \
    X(rret)                  \
    X(jmp_s)                  \
    X(loop_s)                  \
    X(br_false_s)            \
    X(br_true_s)             \
    X(br_icmpeq_s)               \
    X(br_icmpne_s)               \
    X(br_icmpge_s)               \
    X(br_icmpgt_s)               \
    X(br_icmple_s)               \
    X(br_icmplt_s)               \
    X(br_eq_s)             \
    X(br_ne_s)             \
    X(br_ge_s)             \
    X(br_gt_s)             \
    X(br_le_s)             \
    X(br_lt_s)                   \
    X(jmp)                   \
    X(loop)                   \
    X(br_false)              \
    X(br_true)               \
    X(br_icmpeq)                 \
    X(br_icmpne)                 \
    X(br_icmpge)                 \
    X(br_icmpgt)                 \
    X(br_icmple)                 \
    X(br_icmplt)                 \
    X(br_eq)                 \
    X(br_ne)                 \
    X(br_ge)                 \
    X(br_gt)                 \
    X(br_le)                 \
    X(br_lt)                 \
    X(swch)                 \
    X(iadd)               \
    X(isub)               \
    X(imul)               \
    X(uimul)               \
    X(idiv)               \
    X(uidiv)               \
    X(irem)               \
    X(uirem)               \
    X(ladd)               \
    X(lsub)               \
    X(lmul)               \
    X(ulmul)               \
    X(ldiv)               \
    X(uldiv)               \
    X(lrem)               \
    X(ulrem)               \
    X(fadd)               \
    X(fsub)               \
    X(fmul)               \
    X(fdiv)               \
    X(dadd)               \
    X(dsub)               \
    X(dmul)               \
    X(ddiv)               \
    X(lcmp)               \
    X(fcmpl)               \
    X(fcmpg)               \
    X(dcmpl)               \
    X(dcmpg)               \
    X(band)                  \
    X(bor)                   \
    X(bxor)                  \
    X(bshl)                  \
    X(bshr)                  \
    X(bshr_un)               \
    X(bneg)                  \
    X(bnot)                  \
    X(ldstr)

namespace rx {
enum class OpCode : u8 {
#define X(val) val,
    OPCODE_LIST(X)
#undef X
    _count,
    invalid = 255
};

extern const char* g_opcode_str[(u32)OpCode::_count];

constexpr OpCode opcode_add(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return OpCode::iadd;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return OpCode::ladd;
    case PrimTypeKind::F32:
        return OpCode::fadd;
    case PrimTypeKind::F64:
        return OpCode::dadd;
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_sub(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return OpCode::isub;
    case PrimTypeKind::U64:
    case PrimTypeKind::I64:
        return OpCode::lsub;
    case PrimTypeKind::F32:
        return OpCode::fsub;
    case PrimTypeKind::F64:
        return OpCode::dsub;
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_mul(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
        return OpCode::uimul;
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return OpCode::imul;
    case PrimTypeKind::U64:
        return OpCode::ulmul;
    case PrimTypeKind::I64:
        return OpCode::lmul;
    case PrimTypeKind::F32:
        return OpCode::fmul;
    case PrimTypeKind::F64:
        return OpCode::dmul;
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_div(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::Bool:
        return OpCode::invalid;
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
        return OpCode::uidiv;
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return OpCode::idiv;
    case PrimTypeKind::U64:
        return OpCode::uldiv;
    case PrimTypeKind::I64:
        return OpCode::ldiv;
    case PrimTypeKind::F32:
        return OpCode::fdiv;
    case PrimTypeKind::F64:
        return OpCode::ddiv;
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_rem(PrimTypeKind kind) {
    switch (kind) {
    case PrimTypeKind::U8:
    case PrimTypeKind::U16:
    case PrimTypeKind::U32:
        return OpCode::uirem;
    case PrimTypeKind::I8:
    case PrimTypeKind::I16:
    case PrimTypeKind::I32:
        return OpCode::irem;
    case PrimTypeKind::U64:
        return OpCode::ulrem;
    case PrimTypeKind::I64:
        return OpCode::lrem;
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_arithmetic(PrimTypeKind kind, TokenType type) {
    switch (type) {
    case TokenType::Plus:
        return opcode_add(kind);
    case TokenType::Minus:
        return opcode_sub(kind);
    case TokenType::Star:
        return opcode_mul(kind);
    case TokenType::Slash:
        return opcode_div(kind);
    case TokenType::Percent:
        return opcode_rem(kind);
    default:
        return OpCode::invalid;
    }
}

constexpr OpCode opcode_integer_br_cmp(TokenType type, bool shortened, bool opposite = false) {
    if (opposite) {
        switch (type) {
        case TokenType::EqualEqual:
            return shortened ? OpCode::br_icmpne_s : OpCode::br_icmpne;
        case TokenType::BangEqual:
            return shortened ? OpCode::br_icmpeq_s : OpCode::br_icmpeq;
        case TokenType::Less:
            return shortened ? OpCode::br_icmpge_s : OpCode::br_icmpge;
        case TokenType::LessEqual:
            return shortened ? OpCode::br_icmpgt_s : OpCode::br_icmpgt;
        case TokenType::Greater:
            return shortened ? OpCode::br_icmple_s : OpCode::br_icmple;
        case TokenType::GreaterEqual:
            return shortened ? OpCode::br_icmplt_s : OpCode::br_icmplt;
        default:
            return OpCode::invalid;
        }
    }
    else {
        switch (type) {
        case TokenType::EqualEqual:
            return shortened ? OpCode::br_icmpeq_s : OpCode::br_icmpeq;
        case TokenType::BangEqual:
            return shortened ? OpCode::br_icmpne_s : OpCode::br_icmpne;
        case TokenType::Less:
            return shortened ? OpCode::br_icmplt_s : OpCode::br_icmplt;
        case TokenType::LessEqual:
            return shortened ? OpCode::br_icmple_s : OpCode::br_icmple;
        case TokenType::Greater:
            return shortened ? OpCode::br_icmpgt_s : OpCode::br_icmpgt;
        case TokenType::GreaterEqual:
            return shortened ? OpCode::br_icmpge_s : OpCode::br_icmpge;
        default:
            return OpCode::invalid;
        }
    }
}

constexpr OpCode opcode_floating_cmp(PrimTypeKind kind, bool greater) {
    if (kind == PrimTypeKind::F32) {
        return greater ? OpCode::fcmpg : OpCode::fcmpl;
    }
    else if (kind == PrimTypeKind::F64) {
        return greater ? OpCode::dcmpg : OpCode::dcmpl;
    }
    else {
        return OpCode::invalid;
    }
}
}

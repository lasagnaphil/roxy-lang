#pragma once

#include "roxy/core/types.hpp"

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
    X(lstore_s)          \
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
    X(iconst_S)             \
    X(iconst)                \
    X(lconst)                \
    X(fconst)                \
    X(dconst)                \
    X(dup)                  \
    X(pop)                  \
    X(jmp)                  \
    X(call)                 \
    X(callI)                \
    X(ret)                  \
    X(br_s)                 \
    X(brfalse_s)            \
    X(brtrue_s)             \
    X(breq_s)               \
    X(brge_s)               \
    X(brgt_s)               \
    X(brle_s)               \
    X(brlt_s)               \
    X(brne_un_s)             \
    X(brge_un_s)             \
    X(brgt_un_s)             \
    X(brle_un_s)             \
    X(brlt_un_s)             \
    X(br)                   \
    X(brfalse)              \
    X(brtrue)               \
    X(breq)                 \
    X(brge)                 \
    X(brgt)                 \
    X(brle)                 \
    X(brlt)                 \
    X(breq_un)              \
    X(brge_un)              \
    X(brgt_un)              \
    X(brle_un)              \
    X(brlt_un)              \
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
    X(band)                  \
    X(bor)                   \
    X(bxor)                  \
    X(bshl)                  \
    X(bshr)                  \
    X(bshr_un)               \
    X(bneg)                  \
    X(bnot)                  \
    X(conv_i1)              \
    X(conv_u1)              \
    X(conv_i2)              \
    X(conv_u2)              \
    X(conv_i4)              \
    X(conv_u4)              \
    X(conv_i8)              \
    X(conv_u8)              \
    X(conv_r4)              \
    X(conv_r8)              \
    X(ldstr)                \
    X(print)                \

namespace rx {

enum class OpCode : u8 {
#define X(val) val,
    OPCODE_LIST(X)
#undef X
    _count,
    Invalid = 255
};

extern const char* g_opcode_str[(u32)OpCode::_count];

}
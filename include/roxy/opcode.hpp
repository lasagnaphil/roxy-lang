#pragma once

#include "roxy/core/types.hpp"

// IL Opcode list for the Roxy language.

#define OPCODE_LIST(X)      \
    X(nop)                  \
    X(brk)                \
    X(ildarg_0)             \
    X(ildarg_1)             \
    X(ildarg_2)             \
    X(ildarg_3)             \
    X(ildloc_0)             \
    X(ildloc_1)             \
    X(ildloc_2)             \
    X(ildloc_3)             \
    X(istloc_0)             \
    X(istloc_1)             \
    X(istloc_2)             \
    X(istloc_3)             \
    X(ildarg_s)             \
    X(istarg_s)             \
    X(ildloc)              \
    X(ildloc_s)             \
    X(istloc)             \
    X(istloc_s)             \
    X(lldarg_0)          \
    X(lldarg_1)          \
    X(lldarg_2)          \
    X(lldarg_3)          \
    X(lldloc_0)          \
    X(lldloc_1)          \
    X(lldloc_2)          \
    X(lldloc_3)          \
    X(lstloc_0)          \
    X(lstloc_1)          \
    X(lstloc_2)          \
    X(lstloc_3)          \
    X(lldarg_s)          \
    X(lstarg_s)          \
    X(lldloc)            \
    X(lldloc_s)          \
    X(lstloc)            \
    X(lstloc_s)          \
    X(ldcnil)             \
    X(ildc_m1)            \
    X(ildc_0)             \
    X(ildc_1)             \
    X(ildc_2)             \
    X(ildc_3)             \
    X(ildc_4)             \
    X(ildc_5)             \
    X(ildc_6)             \
    X(ildc_7)             \
    X(ildc_8)             \
    X(ildc_S)             \
    X(ildc)                \
    X(lldc)                \
    X(fldc)                \
    X(dldc)                \
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
#pragma once

#include "roxy/type.hpp"

namespace rx {

enum class ILOperandKind : u8 {
    Invalid,
    Address,
    Register,
    ConstInt,
    ConstLong,
    ConstFloat,
    ConstDouble
};

struct ILAddress {
    union {
        u64 addr;
        u16 reg;
        i32 value_i;
        i64 value_l;
        f32 value_f;
        f64 value_d;
    };
    // OperandKind kind;

    static ILAddress make_invalid() { return {.addr = UINT64_MAX}; }

    static ILAddress make_addr(u64 addr) {
        // return {.addr = addr, .kind = ILOperandKind::Address};
        return {.addr = addr};
    }

    static ILAddress make_reg(u16 reg) {
        // return {.reg = reg, .kind = ILOperandKind::Register};
        return {.reg = reg};
    }

    static ILAddress make_const_int(i32 value) {
        // return {.value_i = value, .kind = ILOperandKind::ConstInt};
        return {.value_i = value};
    }

    static ILAddress make_const_long(i64 value) {
        // return {.value_l = value, .kind = ILOperandKind::ConstLong};
        return {.value_l = value};
    }

    static ILAddress make_const_float(f32 value) {
        // return {.value_f = value, .kind = ILOperandKind::ConstFloat};
        return {.value_f = value};
    }

    static ILAddress make_const_double(f64 value) {
        // return {.value_d = value, .kind = ILOperandKind::ConstDouble};
        return {.value_d = value};
    }
};

enum class ILOperator : u8 {
    Invalid,

    // a := b
    AssignI,
    AssignL,
    AssignF,
    AssignD,
    AssignR,

    // a := b + c
    AddI,
    AddL,
    AddF,
    AddD,

    // a := b - c
    SubI,
    SubL,
    SubF,
    SubD,

    // a := b * c
    MulI,
    MulUI,
    MulL,
    MulUL,
    MulF,
    MulD,

    // a := b / c
    DivI,
    DivUI,
    DivL,
    DivUL,
    DivF,
    DivD,

    // a := b % c
    ModI,
    ModL,

    // a := -b
    NegI,
    NegL,
    NegF,
    NegD,

    // a := !b
    Not,

    // a := b & c
    BAndI,
    BAndL,
    // a := b | c
    BOrI,
    BOrL,
    // a := b ^ c
    BXorI,
    BXorL,
    // a := b << c
    BShlI,
    BShlL,
    // a := b >> c
    BShrI,
    BShrL,
    // a := ~b
    BNotI,

    // a := b == c
    EqI,
    EqL,
    EqF,
    EqD,

    // a := b < c
    LtI,
    LtL,
    LtF,
    LtD,

    // a := b <= c
    LeI,
    LeL,
    LeF,
    LeD,

    // ifz a goto b
    IfZ,

    // jmp a
    Jmp,

    // push a
    PushI,
    PushL,
    PushF,
    PushD,

    // pop a
    PopI,
    PopL,
    PopF,
    PopD,

    // call a
    Call,

    // a = phi(b, c)
    Phi,

    // Ret
    Ret,
    RetI,
    RetL,
    RetF,
    RetD
};

struct ILCode {
    ILOperator op;
    ILAddress a1;
    ILAddress a2;
    ILAddress a3;
};

}
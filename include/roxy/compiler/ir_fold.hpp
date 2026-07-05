#pragma once

#include "roxy/core/types.hpp"
#include "roxy/compiler/ssa_ir.hpp"
#include "roxy/compiler/types.hpp"

namespace rx {

// Pure compile-time evaluation for Phase-1 constant folding (see
// docs/internals/optimization.md). These inspect already-materialized
// constant operand instructions and compute the folded constant VALUE; the
// caller (IRBuilder during emission, or a future optimization pass) is
// responsible for emitting it. Each returns false when the fold doesn't
// apply (non-constant operand, unsupported op, or a guarded case like
// division by zero / INT64_MIN / -1, which is left to trap at runtime).

struct FoldedConst {
    enum class Kind { Int, Bool, Float } kind;
    i64 int_val = 0;
    bool bool_val = false;
    f64 float_val = 0.0;  // f32 results are widened; the emitter narrows by result type
};

bool fold_binary_const(IROp op, const IRInst* left, const IRInst* right, FoldedConst& out);
bool fold_unary_const(IROp op, const IRInst* operand, FoldedConst& out);
bool fold_cast_const(const IRInst* source, const Type* source_type, const Type* target_type,
                     FoldedConst& out);

}

#include "roxy/compiler/ssa_ir.hpp"

#include "roxy/core/fmt/format.h"

namespace rx {

const char* ir_op_to_string(IROp op) {
    switch (op) {
        case IROp::ConstNull:   return "const_null";
        case IROp::ConstBool:   return "const_bool";
        case IROp::ConstInt:    return "const_int";
        case IROp::ConstFloat:  return "const_float";
        case IROp::ConstString: return "const_string";

        case IROp::AddI: return "add_i";
        case IROp::SubI: return "sub_i";
        case IROp::MulI: return "mul_i";
        case IROp::DivI: return "div_i";
        case IROp::ModI: return "mod_i";
        case IROp::NegI: return "neg_i";

        case IROp::AddF: return "add_f";
        case IROp::SubF: return "sub_f";
        case IROp::MulF: return "mul_f";
        case IROp::DivF: return "div_f";
        case IROp::NegF: return "neg_f";

        case IROp::EqI: return "eq_i";
        case IROp::NeI: return "ne_i";
        case IROp::LtI: return "lt_i";
        case IROp::LeI: return "le_i";
        case IROp::GtI: return "gt_i";
        case IROp::GeI: return "ge_i";

        case IROp::EqF: return "eq_f";
        case IROp::NeF: return "ne_f";
        case IROp::LtF: return "lt_f";
        case IROp::LeF: return "le_f";
        case IROp::GtF: return "gt_f";
        case IROp::GeF: return "ge_f";

        case IROp::Not:    return "not";
        case IROp::And:    return "and";
        case IROp::Or:     return "or";

        case IROp::BitAnd: return "bit_and";
        case IROp::BitOr:  return "bit_or";
        case IROp::BitNot: return "bit_not";

        case IROp::I2F: return "i2f";
        case IROp::F2I: return "f2i";
        case IROp::I2B: return "i2b";
        case IROp::B2I: return "b2i";

        case IROp::StackAlloc: return "stack_alloc";
        case IROp::GetField: return "get_field";
        case IROp::GetFieldAddr: return "get_field_addr";
        case IROp::SetField: return "set_field";
        case IROp::GetIndex: return "get_index";
        case IROp::SetIndex: return "set_index";

        case IROp::RefInc:    return "ref_inc";
        case IROp::RefDec:    return "ref_dec";
        case IROp::WeakCheck: return "weak_check";

        case IROp::New:    return "new";
        case IROp::Delete: return "delete";

        case IROp::Call:       return "call";
        case IROp::CallNative: return "call_native";

        case IROp::BlockArg: return "block_arg";
        case IROp::Copy:     return "copy";
    }
    return "unknown";
}

static void append_str(Vector<char>& out, const char* str) {
    while (*str) {
        out.push_back(*str++);
    }
}

static void append_value_id(Vector<char>& out, ValueId v) {
    if (!v.is_valid()) {
        append_str(out, "v?");
        return;
    }
    auto s = fmt::format("v{}", v.id);
    for (char c : s) {
        out.push_back(c);
    }
}

static void append_block_id(Vector<char>& out, BlockId b) {
    if (!b.is_valid()) {
        append_str(out, "b?");
        return;
    }
    auto s = fmt::format("b{}", b.id);
    for (char c : s) {
        out.push_back(c);
    }
}

void ir_inst_to_string(const IRInst* inst, Vector<char>& out) {
    // Result = op operands
    append_value_id(out, inst->result);
    append_str(out, " = ");
    append_str(out, ir_op_to_string(inst->op));

    switch (inst->op) {
        case IROp::ConstNull:
            append_str(out, " null");
            break;

        case IROp::ConstBool:
            append_str(out, inst->const_data.bool_val ? " true" : " false");
            break;

        case IROp::ConstInt: {
            auto s = fmt::format(" {}", inst->const_data.int_val);
            for (char c : s) out.push_back(c);
            break;
        }

        case IROp::ConstFloat: {
            auto s = fmt::format(" {}", inst->const_data.float_val);
            for (char c : s) out.push_back(c);
            break;
        }

        case IROp::ConstString:
            append_str(out, " \"");
            for (u32 i = 0; i < inst->const_data.string_val.size(); i++) {
                out.push_back(inst->const_data.string_val[i]);
            }
            append_str(out, "\"");
            break;

        case IROp::AddI:
        case IROp::SubI:
        case IROp::MulI:
        case IROp::DivI:
        case IROp::ModI:
        case IROp::AddF:
        case IROp::SubF:
        case IROp::MulF:
        case IROp::DivF:
        case IROp::EqI:
        case IROp::NeI:
        case IROp::LtI:
        case IROp::LeI:
        case IROp::GtI:
        case IROp::GeI:
        case IROp::EqF:
        case IROp::NeF:
        case IROp::LtF:
        case IROp::LeF:
        case IROp::GtF:
        case IROp::GeF:
        case IROp::And:
        case IROp::Or:
        case IROp::BitAnd:
        case IROp::BitOr:
            append_str(out, " ");
            append_value_id(out, inst->binary.left);
            append_str(out, ", ");
            append_value_id(out, inst->binary.right);
            break;

        case IROp::NegI:
        case IROp::NegF:
        case IROp::Not:
        case IROp::BitNot:
        case IROp::I2F:
        case IROp::F2I:
        case IROp::I2B:
        case IROp::B2I:
        case IROp::RefInc:
        case IROp::RefDec:
        case IROp::WeakCheck:
        case IROp::Delete:
        case IROp::Copy:
            append_str(out, " ");
            append_value_id(out, inst->unary);
            break;

        case IROp::StackAlloc: {
            auto s = fmt::format(" {}", inst->stack_alloc.slot_count);
            for (char c : s) out.push_back(c);
            break;
        }

        case IROp::GetField:
            append_str(out, " ");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (u32 i = 0; i < inst->field.field_name.size(); i++) {
                out.push_back(inst->field.field_name[i]);
            }
            break;

        case IROp::GetFieldAddr:
            append_str(out, " &");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (u32 i = 0; i < inst->field.field_name.size(); i++) {
                out.push_back(inst->field.field_name[i]);
            }
            break;

        case IROp::SetField:
            append_str(out, " ");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (u32 i = 0; i < inst->field.field_name.size(); i++) {
                out.push_back(inst->field.field_name[i]);
            }
            append_str(out, " <- ");
            append_value_id(out, inst->store_value);
            break;

        case IROp::GetIndex:
            append_str(out, " ");
            append_value_id(out, inst->index.object);
            append_str(out, "[");
            append_value_id(out, inst->index.index);
            append_str(out, "]");
            break;

        case IROp::SetIndex:
            append_str(out, " ");
            append_value_id(out, inst->index.object);
            append_str(out, "[");
            append_value_id(out, inst->index.index);
            append_str(out, "] <- ");
            append_value_id(out, inst->store_value);
            break;

        case IROp::New:
            append_str(out, " ");
            for (u32 i = 0; i < inst->new_data.type_name.size(); i++) {
                out.push_back(inst->new_data.type_name[i]);
            }
            append_str(out, "(");
            for (u32 i = 0; i < inst->new_data.args.size(); i++) {
                if (i > 0) append_str(out, ", ");
                append_value_id(out, inst->new_data.args[i]);
            }
            append_str(out, ")");
            break;

        case IROp::Call:
        case IROp::CallNative:
            append_str(out, " ");
            for (u32 i = 0; i < inst->call.func_name.size(); i++) {
                out.push_back(inst->call.func_name[i]);
            }
            append_str(out, "(");
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                if (i > 0) append_str(out, ", ");
                append_value_id(out, inst->call.args[i]);
            }
            append_str(out, ")");
            break;

        case IROp::BlockArg: {
            auto s = fmt::format(" #{}", inst->block_arg_index);
            for (char c : s) out.push_back(c);
            break;
        }
    }
}

static void append_jump_target(Vector<char>& out, const JumpTarget& target) {
    append_block_id(out, target.block);
    if (target.args.size() > 0) {
        append_str(out, "(");
        for (u32 i = 0; i < target.args.size(); i++) {
            if (i > 0) append_str(out, ", ");
            append_value_id(out, target.args[i].value);
        }
        append_str(out, ")");
    }
}

void ir_block_to_string(const IRBlock* block, Vector<char>& out) {
    // Block header: b0(v0, v1):
    append_block_id(out, block->id);
    if (!block->name.empty()) {
        append_str(out, " [");
        for (u32 i = 0; i < block->name.size(); i++) {
            out.push_back(block->name[i]);
        }
        append_str(out, "]");
    }
    if (!block->params.empty()) {
        append_str(out, "(");
        for (u32 i = 0; i < block->params.size(); i++) {
            if (i > 0) append_str(out, ", ");
            append_value_id(out, block->params[i].value);
            if (!block->params[i].name.empty()) {
                append_str(out, ":");
                for (u32 j = 0; j < block->params[i].name.size(); j++) {
                    out.push_back(block->params[i].name[j]);
                }
            }
        }
        append_str(out, ")");
    }
    append_str(out, ":\n");

    // Instructions
    for (const IRInst* inst : block->instructions) {
        append_str(out, "    ");
        ir_inst_to_string(inst, out);
        out.push_back('\n');
    }

    // Terminator
    append_str(out, "    ");
    switch (block->terminator.kind) {
        case TerminatorKind::None:
            append_str(out, "<no terminator>");
            break;

        case TerminatorKind::Goto:
            append_str(out, "goto ");
            append_jump_target(out, block->terminator.goto_target);
            break;

        case TerminatorKind::Branch:
            append_str(out, "if ");
            append_value_id(out, block->terminator.branch.condition);
            append_str(out, " goto ");
            append_jump_target(out, block->terminator.branch.then_target);
            append_str(out, " else ");
            append_jump_target(out, block->terminator.branch.else_target);
            break;

        case TerminatorKind::Return:
            append_str(out, "return");
            if (block->terminator.return_value.is_valid()) {
                append_str(out, " ");
                append_value_id(out, block->terminator.return_value);
            }
            break;

        case TerminatorKind::Unreachable:
            append_str(out, "unreachable");
            break;
    }
    out.push_back('\n');
}

void ir_function_to_string(const IRFunction* func, Vector<char>& out) {
    append_str(out, "fn ");
    for (u32 i = 0; i < func->name.size(); i++) {
        out.push_back(func->name[i]);
    }
    append_str(out, "(");
    for (u32 i = 0; i < func->params.size(); i++) {
        if (i > 0) append_str(out, ", ");
        append_value_id(out, func->params[i].value);
        if (!func->params[i].name.empty()) {
            append_str(out, ":");
            for (u32 j = 0; j < func->params[i].name.size(); j++) {
                out.push_back(func->params[i].name[j]);
            }
        }
    }
    append_str(out, ")");

    if (func->return_type && !func->return_type->is_void()) {
        append_str(out, " -> ");
        Vector<char> type_str;
        type_to_string(func->return_type, type_str);
        for (char c : type_str) out.push_back(c);
    }
    append_str(out, " {\n");

    for (const IRBlock* block : func->blocks) {
        ir_block_to_string(block, out);
        out.push_back('\n');
    }

    append_str(out, "}\n");
}

void ir_module_to_string(const IRModule* module, Vector<char>& out) {
    if (!module->name.empty()) {
        append_str(out, "// Module: ");
        for (u32 i = 0; i < module->name.size(); i++) {
            out.push_back(module->name[i]);
        }
        out.push_back('\n');
        out.push_back('\n');
    }

    for (const IRFunction* func : module->functions) {
        ir_function_to_string(func, out);
        out.push_back('\n');
    }
}

}

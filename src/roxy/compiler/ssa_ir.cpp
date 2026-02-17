#include "roxy/compiler/ssa_ir.hpp"

#include "roxy/core/static_string.hpp"
#include <cstring>

namespace rx {

const char* ir_op_to_string(IROp op) {
    switch (op) {
        case IROp::ConstNull:   return "const_null";
        case IROp::ConstBool:   return "const_bool";
        case IROp::ConstInt:    return "const_int";
        case IROp::ConstF:      return "const_f";
        case IROp::ConstD:      return "const_d";
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

        case IROp::AddD: return "add_d";
        case IROp::SubD: return "sub_d";
        case IROp::MulD: return "mul_d";
        case IROp::DivD: return "div_d";
        case IROp::NegD: return "neg_d";

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

        case IROp::EqD: return "eq_d";
        case IROp::NeD: return "ne_d";
        case IROp::LtD: return "lt_d";
        case IROp::LeD: return "le_d";
        case IROp::GtD: return "gt_d";
        case IROp::GeD: return "ge_d";

        case IROp::Not:    return "not";
        case IROp::And:    return "and";
        case IROp::Or:     return "or";

        case IROp::BitAnd: return "bit_and";
        case IROp::BitOr:  return "bit_or";
        case IROp::BitXor: return "bit_xor";
        case IROp::BitNot: return "bit_not";
        case IROp::Shl:    return "shl";
        case IROp::Shr:    return "shr";

        case IROp::I_TO_F64: return "i_to_f64";
        case IROp::F64_TO_I: return "f64_to_i";
        case IROp::I_TO_B:   return "i_to_b";
        case IROp::B_TO_I:   return "b_to_i";

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

        case IROp::Call:         return "call";
        case IROp::CallNative:   return "call_native";
        case IROp::CallExternal: return "call_external";

        case IROp::BlockArg: return "block_arg";
        case IROp::Copy:     return "copy";

        case IROp::StructCopy: return "struct_copy";

        case IROp::LoadPtr:  return "load_ptr";
        case IROp::StorePtr: return "store_ptr";
        case IROp::VarAddr:  return "var_addr";

        case IROp::Cast:     return "cast";
    }
    return "unknown";
}

static void append_str(Vector<char>& out, const char* str) {
    while (*str) {
        out.push_back(*str++);
    }
}

static void append_string_view(Vector<char>& out, StringView sv) {
    for (char c : sv) {
        out.push_back(c);
    }
}

static void append_value_id(Vector<char>& out, ValueId v) {
    if (!v.is_valid()) {
        append_str(out, "v?");
        return;
    }
    StaticString<32> tmp;
    format_to(tmp, "v{}", v.id);
    for (char c : tmp) out.push_back(c);
}

static void append_block_id(Vector<char>& out, BlockId b) {
    if (!b.is_valid()) {
        append_str(out, "b?");
        return;
    }
    StaticString<32> tmp;
    format_to(tmp, "b{}", b.id);
    for (char c : tmp) out.push_back(c);
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
            StaticString<32> tmp;
            format_to(tmp, " {}", inst->const_data.int_val);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::ConstF: {
            StaticString<32> tmp;
            format_to(tmp, " {}f", inst->const_data.f32_val);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::ConstD: {
            StaticString<32> tmp;
            format_to(tmp, " {}", inst->const_data.f64_val);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::ConstString:
            append_str(out, " \"");
            for (char c : inst->const_data.string_val) {
                out.push_back(c);
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
        case IROp::AddD:
        case IROp::SubD:
        case IROp::MulD:
        case IROp::DivD:
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
        case IROp::EqD:
        case IROp::NeD:
        case IROp::LtD:
        case IROp::LeD:
        case IROp::GtD:
        case IROp::GeD:
        case IROp::And:
        case IROp::Or:
        case IROp::BitAnd:
        case IROp::BitOr:
        case IROp::BitXor:
        case IROp::Shl:
        case IROp::Shr:
            append_str(out, " ");
            append_value_id(out, inst->binary.left);
            append_str(out, ", ");
            append_value_id(out, inst->binary.right);
            break;

        case IROp::NegI:
        case IROp::NegF:
        case IROp::NegD:
        case IROp::Not:
        case IROp::BitNot:
        case IROp::I_TO_F64:
        case IROp::F64_TO_I:
        case IROp::I_TO_B:
        case IROp::B_TO_I:
        case IROp::RefInc:
        case IROp::RefDec:
        case IROp::WeakCheck:
        case IROp::Delete:
        case IROp::Copy:
            append_str(out, " ");
            append_value_id(out, inst->unary);
            break;

        case IROp::StackAlloc: {
            StaticString<32> tmp;
            format_to(tmp, " {}", inst->stack_alloc.slot_count);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::GetField:
            append_str(out, " ");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (char c : inst->field.field_name) {
                out.push_back(c);
            }
            break;

        case IROp::GetFieldAddr:
            append_str(out, " &");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (char c : inst->field.field_name) {
                out.push_back(c);
            }
            break;

        case IROp::SetField:
            append_str(out, " ");
            append_value_id(out, inst->field.object);
            append_str(out, ".");
            for (char c : inst->field.field_name) {
                out.push_back(c);
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
            for (char c : inst->new_data.type_name) {
                out.push_back(c);
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
            for (char c : inst->call.func_name) {
                out.push_back(c);
            }
            append_str(out, "(");
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                if (i > 0) append_str(out, ", ");
                append_value_id(out, inst->call.args[i]);
            }
            append_str(out, ")");
            break;

        case IROp::CallExternal:
            append_str(out, " ");
            append_string_view(out, inst->call_external.module_name);
            append_str(out, ".");
            append_string_view(out, inst->call_external.func_name);
            append_str(out, "(");
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                if (i > 0) append_str(out, ", ");
                append_value_id(out, inst->call_external.args[i]);
            }
            append_str(out, ")");
            break;

        case IROp::BlockArg: {
            StaticString<32> tmp;
            format_to(tmp, " #{}", inst->block_arg_index);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::StructCopy: {
            append_str(out, " ");
            append_value_id(out, inst->struct_copy.dest_ptr);
            append_str(out, " <- ");
            append_value_id(out, inst->struct_copy.source_ptr);
            StaticString<32> tmp;
            format_to(tmp, " ({})", inst->struct_copy.slot_count);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::LoadPtr: {
            append_str(out, " *");
            append_value_id(out, inst->load_ptr.ptr);
            StaticString<32> tmp;
            format_to(tmp, " ({})", inst->load_ptr.slot_count);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::StorePtr: {
            append_str(out, " *");
            append_value_id(out, inst->store_ptr.ptr);
            append_str(out, " = ");
            append_value_id(out, inst->store_ptr.value);
            StaticString<32> tmp;
            format_to(tmp, " ({})", inst->store_ptr.slot_count);
            for (char c : tmp) out.push_back(c);
            break;
        }

        case IROp::VarAddr: {
            append_str(out, " &");
            append_string_view(out, inst->var_addr.name);
            break;
        }

        case IROp::Cast: {
            append_str(out, " ");
            append_value_id(out, inst->cast.source);
            append_str(out, " (");
            if (inst->cast.source_type) {
                Vector<char> type_str;
                type_to_string(inst->cast.source_type, type_str);
                for (char ch : type_str) out.push_back(ch);
            } else {
                append_str(out, "?");
            }
            append_str(out, " -> ");
            if (inst->type) {
                Vector<char> type_str;
                type_to_string(inst->type, type_str);
                for (char ch : type_str) out.push_back(ch);
            } else {
                append_str(out, "?");
            }
            append_str(out, ")");
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
        for (char c : block->name) {
            out.push_back(c);
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
    for (char c : func->name) {
        out.push_back(c);
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
        for (char c : module->name) {
            out.push_back(c);
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

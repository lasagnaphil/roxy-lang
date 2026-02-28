#include "roxy/compiler/ir_validator.hpp"
#include "roxy/core/format.hpp"

#include <cstring>

namespace rx {

void IRValidator::report_error(const char* message) {
    if (m_has_error) return;
    m_has_error = true;
    m_error = message;
}

template<typename... Args>
void IRValidator::report_error_fmt(const char* fmt, const Args&... args) {
    if (m_has_error) return;
    m_has_error = true;
    format_to(m_error_buf, sizeof(m_error_buf), fmt, args...);
    m_error = m_error_buf;
}

bool IRValidator::validate(IRModule* module) {
    m_has_error = false;
    m_error = nullptr;

    for (u32 i = 0; i < module->functions.size(); i++) {
        if (!module->functions[i]) {
            report_error_fmt("module function[{}] is null", i);
            return false;
        }
        if (!validate_function(module->functions[i])) return false;
    }

    return true;
}

bool IRValidator::validate_function(IRFunction* func) {
    if (func->blocks.empty()) {
        report_error_fmt("function '{}' has no blocks", func->name);
        return false;
    }

    // Validate param_is_ptr size matches params size (when non-empty)
    if (!func->param_is_ptr.empty() && func->param_is_ptr.size() != func->params.size()) {
        report_error_fmt("function '{}': param_is_ptr.size() ({}) != params.size() ({})",
                         func->name, static_cast<u32>(func->param_is_ptr.size()),
                         static_cast<u32>(func->params.size()));
        return false;
    }

    // Validate each function parameter
    for (u32 i = 0; i < func->params.size(); i++) {
        const BlockParam& param = func->params[i];
        if (!param.value.is_valid() || param.value.id >= func->next_value_id) {
            report_error_fmt("function '{}': param[{}] has invalid ValueId {}",
                             func->name, i, param.value.id);
            return false;
        }
        if (!param.type) {
            report_error_fmt("function '{}': param[{}] has null type", func->name, i);
            return false;
        }
    }

    // Validate block ID consistency
    for (u32 i = 0; i < func->blocks.size(); i++) {
        if (func->blocks[i]->id.id != i) {
            report_error_fmt("function '{}': blocks[{}]->id.id == {} (expected {})",
                             func->name, i, func->blocks[i]->id.id, i);
            return false;
        }
    }

    // Validate each block
    for (IRBlock* block : func->blocks) {
        if (!validate_block(func, block)) return false;
    }

    // Validate exception handlers
    u32 num_blocks = static_cast<u32>(func->blocks.size());
    for (u32 i = 0; i < func->exception_handlers.size(); i++) {
        const auto& handler = func->exception_handlers[i];
        if (!handler.try_entry.is_valid() || handler.try_entry.id >= num_blocks) {
            report_error_fmt("function '{}': exception_handler[{}] try_entry block {} out of range",
                             func->name, i, handler.try_entry.id);
            return false;
        }
        if (!handler.try_exit.is_valid() || handler.try_exit.id >= num_blocks) {
            report_error_fmt("function '{}': exception_handler[{}] try_exit block {} out of range",
                             func->name, i, handler.try_exit.id);
            return false;
        }
        if (!handler.handler_block.is_valid() || handler.handler_block.id >= num_blocks) {
            report_error_fmt("function '{}': exception_handler[{}] handler_block {} out of range",
                             func->name, i, handler.handler_block.id);
            return false;
        }
    }

    // Validate finally handlers
    for (u32 i = 0; i < func->finally_handlers.size(); i++) {
        const auto& finally_info = func->finally_handlers[i];
        if (!finally_info.try_entry.is_valid() || finally_info.try_entry.id >= num_blocks) {
            report_error_fmt("function '{}': finally_handler[{}] try_entry block {} out of range",
                             func->name, i, finally_info.try_entry.id);
            return false;
        }
        if (!finally_info.try_exit.is_valid() || finally_info.try_exit.id >= num_blocks) {
            report_error_fmt("function '{}': finally_handler[{}] try_exit block {} out of range",
                             func->name, i, finally_info.try_exit.id);
            return false;
        }
        if (!finally_info.finally_block.is_valid() || finally_info.finally_block.id >= num_blocks) {
            report_error_fmt("function '{}': finally_handler[{}] finally_block {} out of range",
                             func->name, i, finally_info.finally_block.id);
            return false;
        }
    }

    return true;
}

bool IRValidator::validate_block(IRFunction* func, IRBlock* block) {
    // Validate block parameters
    for (u32 i = 0; i < block->params.size(); i++) {
        const BlockParam& param = block->params[i];
        if (!param.value.is_valid() || param.value.id >= func->next_value_id) {
            report_error_fmt("function '{}' block {}: param[{}] has invalid ValueId {}",
                             func->name, block->id.id, i, param.value.id);
            return false;
        }
        if (!param.type) {
            report_error_fmt("function '{}' block {}: param[{}] has null type",
                             func->name, block->id.id, i);
            return false;
        }
    }

    // Validate instructions
    bool has_throw = false;
    for (IRInst* inst : block->instructions) {
        if (!validate_instruction(func, block, inst)) return false;
        if (inst->op == IROp::Throw) has_throw = true;
    }

    // Validate that blocks with Throw have Unreachable terminator
    if (has_throw && block->terminator.kind != TerminatorKind::Unreachable) {
        report_error_fmt("function '{}' block {}: block contains Throw but terminator is not Unreachable",
                         func->name, block->id.id);
        return false;
    }

    // Validate terminator
    if (!validate_terminator(func, block)) return false;

    return true;
}

// Helper: check that a ValueId is valid and in range
static bool value_in_range(ValueId value, u32 next_value_id) {
    return value.is_valid() && value.id < next_value_id;
}

// Void operations: these always have null type even with a valid result ValueId
static bool is_void_op(IROp op) {
    return op == IROp::StructCopy;
}

bool IRValidator::validate_instruction(IRFunction* func, IRBlock* block, IRInst* inst) {
    u32 next_id = func->next_value_id;

    // If the instruction produces a result, validate it
    if (inst->result.is_valid()) {
        if (inst->result.id >= next_id) {
            report_error_fmt("function '{}' block {}: instruction result ValueId {} out of range (next_value_id={})",
                             func->name, block->id.id, inst->result.id, next_id);
            return false;
        }
        if (!inst->type && !is_void_op(inst->op)) {
            report_error_fmt("function '{}' block {}: instruction with result v{} has null type",
                             func->name, block->id.id, inst->result.id);
            return false;
        }
    }

    switch (inst->op) {
        // Binary ops
        case IROp::AddI: case IROp::SubI: case IROp::MulI: case IROp::DivI: case IROp::ModI:
        case IROp::AddF: case IROp::SubF: case IROp::MulF: case IROp::DivF:
        case IROp::AddD: case IROp::SubD: case IROp::MulD: case IROp::DivD:
        case IROp::EqI: case IROp::NeI: case IROp::LtI: case IROp::LeI: case IROp::GtI: case IROp::GeI:
        case IROp::EqF: case IROp::NeF: case IROp::LtF: case IROp::LeF: case IROp::GtF: case IROp::GeF:
        case IROp::EqD: case IROp::NeD: case IROp::LtD: case IROp::LeD: case IROp::GtD: case IROp::GeD:
        case IROp::And: case IROp::Or:
        case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor: case IROp::Shl: case IROp::Shr:
        {
            if (!value_in_range(inst->binary.left, next_id)) {
                report_error_fmt("function '{}' block {}: binary op left operand v{} invalid",
                                 func->name, block->id.id, inst->binary.left.id);
                return false;
            }
            if (!value_in_range(inst->binary.right, next_id)) {
                report_error_fmt("function '{}' block {}: binary op right operand v{} invalid",
                                 func->name, block->id.id, inst->binary.right.id);
                return false;
            }
            break;
        }

        // Unary ops
        case IROp::NegI: case IROp::NegF: case IROp::NegD:
        case IROp::Not: case IROp::BitNot:
        case IROp::Copy: case IROp::Delete:
        case IROp::RefInc: case IROp::RefDec: case IROp::WeakCheck:
        case IROp::I_TO_F64: case IROp::F64_TO_I: case IROp::I_TO_B: case IROp::B_TO_I:
        case IROp::Throw:
        {
            if (!value_in_range(inst->unary, next_id)) {
                report_error_fmt("function '{}' block {}: unary op operand v{} invalid",
                                 func->name, block->id.id, inst->unary.id);
                return false;
            }
            break;
        }

        // Call / CallNative
        case IROp::Call:
        case IROp::CallNative:
        {
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                if (!value_in_range(inst->call.args[i], next_id)) {
                    report_error_fmt("function '{}' block {}: call arg[{}] v{} invalid",
                                     func->name, block->id.id, i, inst->call.args[i].id);
                    return false;
                }
            }
            break;
        }

        // CallExternal
        case IROp::CallExternal:
        {
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                if (!value_in_range(inst->call_external.args[i], next_id)) {
                    report_error_fmt("function '{}' block {}: call_external arg[{}] v{} invalid",
                                     func->name, block->id.id, i, inst->call_external.args[i].id);
                    return false;
                }
            }
            break;
        }

        // Field access
        case IROp::GetField:
        case IROp::GetFieldAddr:
        {
            if (!value_in_range(inst->field.object, next_id)) {
                report_error_fmt("function '{}' block {}: field access object v{} invalid",
                                 func->name, block->id.id, inst->field.object.id);
                return false;
            }
            break;
        }

        // SetField
        case IROp::SetField:
        {
            if (!value_in_range(inst->field.object, next_id)) {
                report_error_fmt("function '{}' block {}: set_field object v{} invalid",
                                 func->name, block->id.id, inst->field.object.id);
                return false;
            }
            if (!value_in_range(inst->store_value, next_id)) {
                report_error_fmt("function '{}' block {}: set_field store_value v{} invalid",
                                 func->name, block->id.id, inst->store_value.id);
                return false;
            }
            break;
        }

        // New
        case IROp::New:
        {
            for (u32 i = 0; i < inst->new_data.args.size(); i++) {
                if (!value_in_range(inst->new_data.args[i], next_id)) {
                    report_error_fmt("function '{}' block {}: new arg[{}] v{} invalid",
                                     func->name, block->id.id, i, inst->new_data.args[i].id);
                    return false;
                }
            }
            break;
        }

        // StructCopy
        case IROp::StructCopy:
        {
            if (!value_in_range(inst->struct_copy.dest_ptr, next_id)) {
                report_error_fmt("function '{}' block {}: struct_copy dest_ptr v{} invalid",
                                 func->name, block->id.id, inst->struct_copy.dest_ptr.id);
                return false;
            }
            if (!value_in_range(inst->struct_copy.source_ptr, next_id)) {
                report_error_fmt("function '{}' block {}: struct_copy source_ptr v{} invalid",
                                 func->name, block->id.id, inst->struct_copy.source_ptr.id);
                return false;
            }
            break;
        }

        // LoadPtr
        case IROp::LoadPtr:
        {
            if (!value_in_range(inst->load_ptr.ptr, next_id)) {
                report_error_fmt("function '{}' block {}: load_ptr ptr v{} invalid",
                                 func->name, block->id.id, inst->load_ptr.ptr.id);
                return false;
            }
            break;
        }

        // StorePtr
        case IROp::StorePtr:
        {
            if (!value_in_range(inst->store_ptr.ptr, next_id)) {
                report_error_fmt("function '{}' block {}: store_ptr ptr v{} invalid",
                                 func->name, block->id.id, inst->store_ptr.ptr.id);
                return false;
            }
            if (!value_in_range(inst->store_ptr.value, next_id)) {
                report_error_fmt("function '{}' block {}: store_ptr value v{} invalid",
                                 func->name, block->id.id, inst->store_ptr.value.id);
                return false;
            }
            break;
        }

        // Cast
        case IROp::Cast:
        {
            if (!value_in_range(inst->cast.source, next_id)) {
                report_error_fmt("function '{}' block {}: cast source v{} invalid",
                                 func->name, block->id.id, inst->cast.source.id);
                return false;
            }
            if (!inst->cast.source_type) {
                report_error_fmt("function '{}' block {}: cast has null source_type",
                                 func->name, block->id.id);
                return false;
            }
            break;
        }

        // BlockArg
        case IROp::BlockArg:
        {
            if (inst->block_arg_index >= block->params.size()) {
                report_error_fmt("function '{}' block {}: block_arg_index {} >= block params size {}",
                                 func->name, block->id.id, inst->block_arg_index,
                                 static_cast<u32>(block->params.size()));
                return false;
            }
            break;
        }

        // Constants, StackAlloc, VarAddr - no operand ValueIds to validate
        case IROp::ConstNull: case IROp::ConstBool: case IROp::ConstInt:
        case IROp::ConstF: case IROp::ConstD: case IROp::ConstString:
        case IROp::StackAlloc:
        case IROp::VarAddr:
            break;
    }

    return true;
}

bool IRValidator::validate_terminator(IRFunction* func, IRBlock* block) {
    const Terminator& term = block->terminator;

    if (term.kind == TerminatorKind::None) {
        report_error_fmt("function '{}' block {}: missing terminator",
                         func->name, block->id.id);
        return false;
    }

    switch (term.kind) {
        case TerminatorKind::Return:
        {
            if (term.return_value.is_valid() && term.return_value.id >= func->next_value_id) {
                report_error_fmt("function '{}' block {}: return value v{} out of range",
                                 func->name, block->id.id, term.return_value.id);
                return false;
            }
            break;
        }

        case TerminatorKind::Goto:
        {
            if (!validate_jump_target(func, term.goto_target, "goto")) return false;
            break;
        }

        case TerminatorKind::Branch:
        {
            if (!value_in_range(term.branch.condition, func->next_value_id)) {
                report_error_fmt("function '{}' block {}: branch condition v{} invalid",
                                 func->name, block->id.id, term.branch.condition.id);
                return false;
            }
            if (!validate_jump_target(func, term.branch.then_target, "branch.then")) return false;
            if (!validate_jump_target(func, term.branch.else_target, "branch.else")) return false;
            break;
        }

        case TerminatorKind::Unreachable:
        case TerminatorKind::None:
            break;
    }

    return true;
}

bool IRValidator::validate_jump_target(IRFunction* func, const JumpTarget& target, const char* label) {
    if (!target.block.is_valid() || target.block.id >= func->blocks.size()) {
        report_error_fmt("function '{}': {} target block {} out of range (num blocks={})",
                         func->name, label, target.block.id,
                         static_cast<u32>(func->blocks.size()));
        return false;
    }

    IRBlock* target_block = func->blocks[target.block.id];
    if (target.args.size() != target_block->params.size()) {
        report_error_fmt("function '{}': {} to block {}: arg count {} != target param count {}",
                         func->name, label, target.block.id,
                         static_cast<u32>(target.args.size()),
                         static_cast<u32>(target_block->params.size()));
        return false;
    }

    for (u32 i = 0; i < target.args.size(); i++) {
        if (!value_in_range(target.args[i].value, func->next_value_id)) {
            report_error_fmt("function '{}': {} to block {}: arg[{}] v{} invalid",
                             func->name, label, target.block.id, i, target.args[i].value.id);
            return false;
        }
    }

    return true;
}

} // namespace rx

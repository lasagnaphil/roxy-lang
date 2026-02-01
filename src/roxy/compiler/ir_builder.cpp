#include "roxy/compiler/ir_builder.hpp"
#include "roxy/compiler/module_registry.hpp"
#include "roxy/vm/binding/registry.hpp"

#include <cassert>
#include <cstring>

namespace rx {

IRBuilder::IRBuilder(BumpAllocator& allocator, TypeCache& types, NativeRegistry& registry,
                     SymbolTable& symbols, ModuleRegistry& module_registry)
    : m_allocator(allocator)
    , m_types(types)
    , m_registry(registry)
    , m_symbols(symbols)
    , m_module_registry(module_registry)
    , m_current_func(nullptr)
    , m_current_block(nullptr)
{
}

IRModule* IRBuilder::build(Program* program) {
    IRModule* module = m_allocator.emplace<IRModule>();

    // Track which struct types have user-defined default constructors
    tsl::robin_map<StringView, bool, StringViewHash, StringViewEqual> has_default_ctor;

    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclFun) {
            if (!decl->fun_decl.is_native && decl->fun_decl.body) {
                IRFunction* func = build_function(&decl->fun_decl);
                module->functions.push_back(func);
            }
        }
        else if (decl->kind == AstKind::DeclStruct) {
            // Build methods
            StructDecl& sd = decl->struct_decl;
            for (u32 j = 0; j < sd.methods.size(); j++) {
                FunDecl* method = sd.methods[j];
                if (method && !method->is_native && method->body) {
                    IRFunction* func = build_function(method);
                    module->functions.push_back(func);
                }
            }
        }
        else if (decl->kind == AstKind::DeclConstructor) {
            // Build constructor
            ConstructorDecl& cd = decl->constructor_decl;
            Type* struct_type = m_types.named_type_by_name(cd.struct_name);
            if (struct_type && cd.body) {
                IRFunction* func = build_constructor(&cd, struct_type);
                module->functions.push_back(func);
                // Track if this is a default constructor (no name)
                if (cd.name.empty()) {
                    has_default_ctor[cd.struct_name] = true;
                }
            }
        }
        else if (decl->kind == AstKind::DeclDestructor) {
            // Build destructor
            DestructorDecl& dd = decl->destructor_decl;
            Type* struct_type = m_types.named_type_by_name(dd.struct_name);
            if (struct_type && dd.body) {
                IRFunction* func = build_destructor(&dd, struct_type);
                module->functions.push_back(func);
            }
        }
        else if (decl->kind == AstKind::DeclMethod) {
            // Build method
            MethodDecl& md = decl->method_decl;
            Type* struct_type = m_types.named_type_by_name(md.struct_name);
            if (struct_type && md.body) {
                IRFunction* func = build_method(&md, struct_type);
                module->functions.push_back(func);
            }
        }
    }

    // Generate synthesized default constructors for structs without user-defined ones
    for (u32 i = 0; i < program->declarations.size(); i++) {
        Decl* decl = program->declarations[i];
        if (!decl) continue;

        if (decl->kind == AstKind::DeclStruct) {
            StructDecl& sd = decl->struct_decl;
            // Check if this struct already has a user-defined default constructor
            if (has_default_ctor.find(sd.name) == has_default_ctor.end()) {
                Type* struct_type = m_types.named_type_by_name(sd.name);
                if (struct_type) {
                    IRFunction* func = build_synthesized_default_constructor(struct_type);
                    module->functions.push_back(func);
                }
            }
        }
    }

    return module;
}

IRFunction* IRBuilder::build_function(FunDecl* decl) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = decl->name;

    // Set up parameters
    setup_parameters(decl->params);

    // Resolve return type
    if (decl->return_type) {
        m_current_func->return_type = m_types.type_by_name(decl->return_type->name);
        if (!m_current_func->return_type) {
            m_current_func->return_type = m_types.void_type();
        }
        m_current_func->return_type = apply_ref_kind(m_current_func->return_type, decl->return_type->ref_kind);
    } else {
        m_current_func->return_type = m_types.void_type();
    }

    // Check for large struct return - add hidden output pointer as last parameter
    if (m_current_func->returns_large_struct()) {
        BlockParam hidden_param;
        hidden_param.value = m_current_func->new_value();
        hidden_param.type = m_current_func->return_type;  // Pointer to struct
        hidden_param.name = "__ret_ptr";
        m_current_func->params.push_back(hidden_param);
        m_current_func->param_is_ptr.push_back(true);
    }

    // Begin function body
    begin_function_body(true);  // skip hidden return pointer

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (u32 i = 0; i < block.declarations.size(); i++) {
            gen_decl(block.declarations[i]);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_constructor(ConstructorDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_constructor(decl->struct_name, decl->name);

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Constructors return void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Check if struct has a parent - if so, we may need to call parent constructor
    Type* parent_type = struct_type->struct_info.parent;

    // Detect if first statement is an explicit super() call
    bool has_explicit_super = false;
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        if (block.declarations.size() > 0) {
            Decl* first_decl = block.declarations[0];
            if (first_decl && first_decl->kind == AstKind::StmtExpr) {
                Expr* first_expr = first_decl->stmt.expr_stmt.expr;
                if (first_expr && first_expr->kind == AstKind::ExprCall) {
                    CallExpr& call = first_expr->call;
                    if (call.callee && call.callee->kind == AstKind::ExprSuper) {
                        has_explicit_super = true;
                    }
                }
            }
        }
    }

    // If struct has parent and no explicit super(), call parent's default constructor
    if (parent_type && !has_explicit_super) {
        StringView parent_ctor_name = mangle_constructor(parent_type->struct_info.name);
        Span<ValueId> ctor_args = alloc_span<ValueId>(1);
        ctor_args[0] = m_current_func->params[0].value;  // 'self' is first parameter
        emit_call(parent_ctor_name, ctor_args, m_types.void_type());
    }

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (u32 i = 0; i < block.declarations.size(); i++) {
            gen_decl(block.declarations[i]);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_destructor(DestructorDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_destructor(decl->struct_name, decl->name);

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Destructors return void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (u32 i = 0; i < block.declarations.size(); i++) {
            gen_decl(block.declarations[i]);
        }
    }

    // After child destructor body, call parent's default destructor if present
    // Only chain default destructors (named destructors are called explicitly)
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type && decl->name.empty()) {
        StringView parent_dtor_name = mangle_destructor(parent_type->struct_info.name);
        Span<ValueId> dtor_args = alloc_span<ValueId>(1);
        dtor_args[0] = m_current_func->params[0].value;  // 'self' is first parameter
        emit_call(parent_dtor_name, dtor_args, m_types.void_type());
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_method(MethodDecl* decl, Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();
    m_current_func->name = mangle_method(decl->struct_name, decl->name);

    // Set up parameters with 'self' as first parameter
    setup_parameters(decl->params, struct_type);

    // Resolve return type
    if (decl->return_type) {
        m_current_func->return_type = m_types.type_by_name(decl->return_type->name);
        if (!m_current_func->return_type) {
            m_current_func->return_type = m_types.void_type();
        }
        m_current_func->return_type = apply_ref_kind(m_current_func->return_type, decl->return_type->ref_kind);
    } else {
        m_current_func->return_type = m_types.void_type();
    }

    // Check for large struct return - add hidden output pointer as last parameter
    if (m_current_func->returns_large_struct()) {
        BlockParam hidden_param;
        hidden_param.value = m_current_func->new_value();
        hidden_param.type = m_current_func->return_type;
        hidden_param.name = "__ret_ptr";
        m_current_func->params.push_back(hidden_param);
        m_current_func->param_is_ptr.push_back(true);
    }

    // Begin function body
    begin_function_body(true);  // skip hidden return pointer

    // Generate body
    if (decl->body && decl->body->kind == AstKind::StmtBlock) {
        BlockStmt& block = decl->body->block;
        for (u32 i = 0; i < block.declarations.size(); i++) {
            gen_decl(block.declarations[i]);
        }
    }

    // End function body
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

IRFunction* IRBuilder::build_synthesized_default_constructor(Type* struct_type) {
    m_current_func = m_allocator.emplace<IRFunction>();

    StructTypeInfo& sti = struct_type->struct_info;
    m_current_func->name = mangle_constructor(sti.name);

    // Set up parameters - only 'self'
    setup_parameters({}, struct_type);

    // Constructor returns void
    m_current_func->return_type = m_types.void_type();

    // Begin function body
    begin_function_body(false);

    // Get 'self' parameter value
    BlockParam& self_param = m_current_func->params[0];

    // If struct has parent, call parent's default constructor first
    Type* parent_type = struct_type->struct_info.parent;
    if (parent_type) {
        StringView parent_ctor_name = mangle_constructor(parent_type->struct_info.name);
        Span<ValueId> ctor_args = alloc_span<ValueId>(1);
        ctor_args[0] = self_param.value;
        emit_call(parent_ctor_name, ctor_args, m_types.void_type());
    }

    // Get the number of inherited fields (from parent)
    u32 inherited_field_count = parent_type ? parent_type->struct_info.fields.size() : 0;

    // Generate field initialization code (only for own fields, not inherited)
    StructDecl& sd = sti.decl->struct_decl;
    ValueId self_ptr = self_param.value;

    for (u32 i = inherited_field_count; i < sti.fields.size(); i++) {
        FieldInfo& fi = sti.fields[i];
        // Adjust index for the StructDecl (which doesn't include inherited fields)
        u32 sd_index = i - inherited_field_count;
        ValueId value;

        // Use default value if defined, otherwise zero-init
        if (sd.fields[sd_index].default_value) {
            value = gen_expr(sd.fields[sd_index].default_value);
        } else {
            // Generate zero value based on field type
            if (fi.type->is_bool()) {
                value = emit_const_bool(false);
            } else if (fi.type->is_integer()) {
                value = emit_const_int(0, fi.type);
            } else if (fi.type->is_float()) {
                value = emit_const_float(0.0, fi.type);
            } else if (fi.type->kind == TypeKind::String) {
                value = emit_const_string("");
            } else if (fi.type->is_struct()) {
                // For nested structs, recursively zero-init
                u32 nested_slots = fi.type->struct_info.slot_count;
                ValueId nested_ptr = emit_stack_alloc(nested_slots, fi.type);
                // Zero the nested struct fields
                StructTypeInfo& nested_sti = fi.type->struct_info;
                StructDecl& nested_sd = nested_sti.decl->struct_decl;
                for (u32 j = 0; j < nested_sti.fields.size(); j++) {
                    FieldInfo& nfi = nested_sti.fields[j];
                    ValueId nval;
                    if (nested_sd.fields[j].default_value) {
                        nval = gen_expr(nested_sd.fields[j].default_value);
                    } else if (nfi.type->is_bool()) {
                        nval = emit_const_bool(false);
                    } else if (nfi.type->is_integer()) {
                        nval = emit_const_int(0, nfi.type);
                    } else if (nfi.type->is_float()) {
                        nval = emit_const_float(0.0, nfi.type);
                    } else {
                        nval = emit_const_null();
                    }
                    emit_set_field(nested_ptr, nfi.name, nfi.slot_offset, nfi.slot_count, nval, nfi.type);
                }
                // Copy nested struct to field
                ValueId field_addr = emit_get_field_addr(self_ptr, fi.name, fi.slot_offset, fi.type);
                emit_struct_copy(field_addr, nested_ptr, nested_slots);
                continue;
            } else {
                // Pointers, references, etc. - null
                value = emit_const_null();
            }
        }

        // For struct-typed fields with default values, use StructCopy
        if (fi.type && fi.type->is_struct() && sd.fields[sd_index].default_value) {
            ValueId field_addr = emit_get_field_addr(self_ptr, fi.name, fi.slot_offset, fi.type);
            emit_struct_copy(field_addr, value, fi.slot_count);
        } else {
            emit_set_field(self_ptr, fi.name, fi.slot_offset, fi.slot_count, value, fi.type);
        }
    }

    // End function body (will add implicit return)
    end_function_body();

    IRFunction* result = m_current_func;
    m_current_func = nullptr;
    m_current_block = nullptr;
    return result;
}

// Block management

IRBlock* IRBuilder::create_block(StringView name) {
    IRBlock* block = m_allocator.emplace<IRBlock>();
    block->id = BlockId{static_cast<u32>(m_current_func->blocks.size())};
    block->name = name;
    m_current_func->blocks.push_back(block);
    return block;
}

void IRBuilder::set_current_block(IRBlock* block) {
    m_current_block = block;
}

void IRBuilder::finish_block_goto(BlockId target, Span<BlockArgPair> args) {
    if (!m_current_block) return;
    m_current_block->terminator.kind = TerminatorKind::Goto;
    m_current_block->terminator.goto_target.block = target;
    m_current_block->terminator.goto_target.args = args;
}

void IRBuilder::finish_block_branch(ValueId cond, BlockId then_block, BlockId else_block,
                                    Span<BlockArgPair> then_args, Span<BlockArgPair> else_args) {
    if (!m_current_block) return;
    m_current_block->terminator.kind = TerminatorKind::Branch;
    m_current_block->terminator.branch.condition = cond;
    m_current_block->terminator.branch.then_target.block = then_block;
    m_current_block->terminator.branch.then_target.args = then_args;
    m_current_block->terminator.branch.else_target.block = else_block;
    m_current_block->terminator.branch.else_target.args = else_args;
}

void IRBuilder::finish_block_return(ValueId value) {
    if (!m_current_block) return;

    // Release ref borrows before returning (constraint reference model)
    emit_ref_param_decrements();

    m_current_block->terminator.kind = TerminatorKind::Return;
    m_current_block->terminator.return_value = value;
}

// Instruction emission

IRInst* IRBuilder::emit_inst(IROp op, Type* result_type) {
    if (!m_current_block) return nullptr;

    IRInst* inst = m_allocator.emplace<IRInst>();
    inst->op = op;
    inst->result = m_current_func->new_value();
    inst->type = result_type;
    m_current_block->instructions.push_back(inst);
    return inst;
}

ValueId IRBuilder::emit_const_null() {
    IRInst* inst = emit_inst(IROp::ConstNull, m_types.nil_type());
    return inst ? inst->result : ValueId::invalid();
}

ValueId IRBuilder::emit_const_bool(bool value) {
    IRInst* inst = emit_inst(IROp::ConstBool, m_types.bool_type());
    if (inst) {
        inst->const_data.bool_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_int(i64 value, Type* type) {
    if (!type) type = m_types.i64_type();
    IRInst* inst = emit_inst(IROp::ConstInt, type);
    if (inst) {
        inst->const_data.int_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_float(f64 value, Type* type) {
    if (!type) type = m_types.f64_type();
    IRInst* inst = emit_inst(IROp::ConstFloat, type);
    if (inst) {
        inst->const_data.float_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_const_string(StringView value) {
    IRInst* inst = emit_inst(IROp::ConstString, m_types.string_type());
    if (inst) {
        inst->const_data.string_val = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_binary(IROp op, ValueId left, ValueId right, Type* result_type) {
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->binary.left = left;
        inst->binary.right = right;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_unary(IROp op, ValueId operand, Type* result_type) {
    IRInst* inst = emit_inst(op, result_type);
    if (inst) {
        inst->unary = operand;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_copy(ValueId value, Type* type) {
    IRInst* inst = emit_inst(IROp::Copy, type);
    if (inst) {
        inst->unary = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call(StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::Call, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = 0;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_native(StringView func_name, Span<ValueId> args, Type* result_type, u8 native_index) {
    IRInst* inst = emit_inst(IROp::CallNative, result_type);
    if (inst) {
        inst->call.func_name = func_name;
        inst->call.args = args;
        inst->call.native_index = native_index;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_call_external(StringView module_name, StringView func_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::CallExternal, result_type);
    if (inst) {
        inst->call_external.module_name = module_name;
        inst->call_external.func_name = func_name;
        inst->call_external.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_new(StringView type_name, Span<ValueId> args, Type* result_type) {
    IRInst* inst = emit_inst(IROp::New, result_type);
    if (inst) {
        inst->new_data.type_name = type_name;
        inst->new_data.args = args;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_stack_alloc(u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StackAlloc, result_type);
    if (inst) {
        inst->stack_alloc.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_field_addr(ValueId object, StringView field_name, u32 slot_offset, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetFieldAddr, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = 0;  // Not used for address computation
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_set_field(ValueId object, StringView field_name, u32 slot_offset, u32 slot_count, ValueId value, Type* result_type) {
    IRInst* inst = emit_inst(IROp::SetField, result_type);
    if (inst) {
        inst->field.object = object;
        inst->field.field_name = field_name;
        inst->field.slot_offset = slot_offset;
        inst->field.slot_count = slot_count;
        inst->store_value = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_get_index(ValueId object, ValueId index, Type* result_type) {
    IRInst* inst = emit_inst(IROp::GetIndex, result_type);
    if (inst) {
        inst->index.object = object;
        inst->index.index = index;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_set_index(ValueId object, ValueId index, ValueId value, Type* result_type) {
    IRInst* inst = emit_inst(IROp::SetIndex, result_type);
    if (inst) {
        inst->index.object = object;
        inst->index.index = index;
        inst->store_value = value;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_load_ptr(ValueId ptr, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::LoadPtr, result_type);
    if (inst) {
        inst->load_ptr.ptr = ptr;
        inst->load_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

ValueId IRBuilder::emit_store_ptr(ValueId ptr, ValueId value, u32 slot_count, Type* result_type) {
    IRInst* inst = emit_inst(IROp::StorePtr, result_type);
    if (inst) {
        inst->store_ptr.ptr = ptr;
        inst->store_ptr.value = value;
        inst->store_ptr.slot_count = slot_count;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_struct_copy(ValueId dest_ptr, ValueId source_ptr, u32 slot_count) {
    IRInst* inst = emit_inst(IROp::StructCopy, nullptr);
    if (inst) {
        inst->struct_copy.dest_ptr = dest_ptr;
        inst->struct_copy.source_ptr = source_ptr;
        inst->struct_copy.slot_count = slot_count;
    }
}

ValueId IRBuilder::emit_var_addr(StringView name, Type* result_type) {
    IRInst* inst = emit_inst(IROp::VarAddr, result_type);
    if (inst) {
        inst->var_addr.name = name;
        return inst->result;
    }
    return ValueId::invalid();
}

void IRBuilder::emit_ref_inc(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefInc, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_ref_dec(ValueId ptr) {
    IRInst* inst = emit_inst(IROp::RefDec, m_types.void_type());
    if (inst) inst->unary = ptr;
}

void IRBuilder::emit_ref_param_decrements() {
    // Emit RefDec for all ref-typed parameters before function exit
    for (ValueId param_val : m_ref_params) {
        emit_ref_dec(param_val);
    }
}

// Statement generation

void IRBuilder::gen_stmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            gen_expr_stmt(stmt);
            break;
        case AstKind::StmtBlock:
            gen_block_stmt(stmt);
            break;
        case AstKind::StmtIf:
            gen_if_stmt(stmt);
            break;
        case AstKind::StmtWhile:
            gen_while_stmt(stmt);
            break;
        case AstKind::StmtFor:
            gen_for_stmt(stmt);
            break;
        case AstKind::StmtReturn:
            gen_return_stmt(stmt);
            break;
        case AstKind::StmtBreak:
            gen_break_stmt(stmt);
            break;
        case AstKind::StmtContinue:
            gen_continue_stmt(stmt);
            break;
        case AstKind::StmtDelete:
            gen_delete_stmt(stmt);
            break;
        default:
            break;
    }
}

void IRBuilder::gen_expr_stmt(Stmt* stmt) {
    gen_expr(stmt->expr_stmt.expr);
}

void IRBuilder::gen_block_stmt(Stmt* stmt) {
    push_scope();

    BlockStmt& block = stmt->block;
    for (u32 i = 0; i < block.declarations.size(); i++) {
        gen_decl(block.declarations[i]);
    }

    pop_scope();
}

void IRBuilder::gen_if_stmt(Stmt* stmt) {
    IfStmt& is = stmt->if_stmt;

    // Evaluate condition
    ValueId cond = gen_expr(is.condition);

    // Collect variables assigned in both branches
    Vector<StringView> then_modified, else_modified;
    collect_assigned_vars(is.then_branch, then_modified);
    if (is.else_branch) {
        collect_assigned_vars(is.else_branch, else_modified);
    }

    // Find variables that are assigned in either branch and exist before the if
    // These need phi nodes (block params) at the merge point
    Vector<StringView> phi_vars;
    for (u32 i = 0; i < then_modified.size(); i++) {
        LocalVar* lv = find_local(then_modified[i]);
        if (lv && lv->value.is_valid()) {
            bool found = false;
            for (u32 j = 0; j < phi_vars.size(); j++) {
                if (phi_vars[j] == then_modified[i]) { found = true; break; }
            }
            if (!found) phi_vars.push_back(then_modified[i]);
        }
    }
    for (u32 i = 0; i < else_modified.size(); i++) {
        LocalVar* lv = find_local(else_modified[i]);
        if (lv && lv->value.is_valid()) {
            bool found = false;
            for (u32 j = 0; j < phi_vars.size(); j++) {
                if (phi_vars[j] == else_modified[i]) { found = true; break; }
            }
            if (!found) phi_vars.push_back(else_modified[i]);
        }
    }

    // Create blocks
    IRBlock* then_block = create_block("then");
    IRBlock* else_block = is.else_branch ? create_block("else") : nullptr;
    IRBlock* merge_block = create_block("endif");

    // Create block params on merge block for phi variables
    struct PhiInfo {
        StringView name;
        Type* type;
        ValueId merge_param;
        ValueId original_value;
    };
    Vector<PhiInfo> phi_info;
    for (u32 i = 0; i < phi_vars.size(); i++) {
        LocalVar* lv = find_local(phi_vars[i]);
        if (lv) {
            ValueId param = m_current_func->new_value();
            merge_block->params.push_back({param, lv->type, phi_vars[i]});
            phi_info.push_back({phi_vars[i], lv->type, param, lv->value});
        }
    }

    // Branch based on condition
    if (else_block) {
        finish_block_branch(cond, then_block->id, else_block->id);
    } else {
        // No else branch - pass original values as args to merge_block
        Vector<BlockArgPair> fallthrough_args;
        for (u32 i = 0; i < phi_info.size(); i++) {
            fallthrough_args.push_back({phi_info[i].original_value});
        }
        finish_block_branch(cond, then_block->id, merge_block->id, {}, alloc_span(fallthrough_args));
    }

    // Save variable state before then branch (so else branch sees original values)
    Vector<tsl::robin_map<StringView, LocalVar, StringViewHash, StringViewEqual>> saved_scopes;
    if (else_block) {
        saved_scopes.reserve(m_local_scopes.size());
        for (auto& scope : m_local_scopes) {
            saved_scopes.push_back(scope);
        }
    }

    // Generate then branch
    set_current_block(then_block);
    gen_stmt(is.then_branch);
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        // Build args for merge block
        Vector<BlockArgPair> then_args;
        for (u32 i = 0; i < phi_info.size(); i++) {
            ValueId val = lookup_local(phi_info[i].name);
            then_args.push_back({val});
        }
        finish_block_goto(merge_block->id, alloc_span(then_args));
    }

    // Generate else branch
    if (else_block) {
        // Restore variable state so else branch sees original values
        m_local_scopes = std::move(saved_scopes);

        set_current_block(else_block);
        gen_stmt(is.else_branch);
        if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
            // Build args for merge block
            Vector<BlockArgPair> else_args;
            for (u32 i = 0; i < phi_info.size(); i++) {
                ValueId val = lookup_local(phi_info[i].name);
                else_args.push_back({val});
            }
            finish_block_goto(merge_block->id, alloc_span(else_args));
        }
    }

    // Continue with merge block
    set_current_block(merge_block);

    // Bind variables to merge block params (phi results)
    for (u32 i = 0; i < phi_info.size(); i++) {
        define_local(phi_info[i].name, phi_info[i].merge_param, phi_info[i].type);
    }
}

void IRBuilder::gen_while_stmt(Stmt* stmt) {
    WhileStmt& ws = stmt->while_stmt;

    // 1. Collect variables assigned in the loop body
    Vector<StringView> modified_vars;
    collect_assigned_vars(ws.body, modified_vars);

    // 2. Create blocks
    IRBlock* header_block = create_block("while");
    IRBlock* body_block = create_block("body");
    IRBlock* exit_block = create_block("endwhile");

    // 3. Create block params for modified vars that exist before the loop
    Vector<LoopVarInfo> loop_vars;
    Vector<BlockArgPair> initial_args;
    for (u32 i = 0; i < modified_vars.size(); i++) {
        StringView name = modified_vars[i];
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            ValueId param = m_current_func->new_value();
            header_block->params.push_back({param, lv->type, name});
            loop_vars.push_back({name, lv->type, param, lv->value});
            initial_args.push_back({lv->value});
        }
    }

    // 4. Jump to header with initial values
    finish_block_goto(header_block->id, alloc_span(initial_args));

    // 5. In header, bind locals to block params
    set_current_block(header_block);
    for (u32 i = 0; i < loop_vars.size(); i++) {
        define_local(loop_vars[i].name, loop_vars[i].header_param, loop_vars[i].type);
    }

    // 6. Condition and branch
    ValueId cond = gen_expr(ws.condition);
    finish_block_branch(cond, body_block->id, exit_block->id);

    // 7. Push loop info for break/continue
    m_loop_stack.push_back({header_block, exit_block, header_block, loop_vars});

    // 8. Generate body
    set_current_block(body_block);
    gen_stmt(ws.body);

    // 9. Back edge with updated values
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        Span<BlockArgPair> back_args = make_loop_args(m_loop_stack.back().loop_vars);
        finish_block_goto(header_block->id, back_args);
    }

    // Save the loop vars before popping (need them for exit block)
    Vector<LoopVarInfo> saved_loop_vars = m_loop_stack.back().loop_vars;  // copy, not move
    m_loop_stack.pop_back();

    // 10. Exit block - use header params as final values
    set_current_block(exit_block);
    for (u32 i = 0; i < saved_loop_vars.size(); i++) {
        define_local(saved_loop_vars[i].name, saved_loop_vars[i].header_param, saved_loop_vars[i].type);
    }
}

void IRBuilder::gen_for_stmt(Stmt* stmt) {
    ForStmt& fs = stmt->for_stmt;

    push_scope();

    // 1. Initialize (creates the loop variable in scope)
    if (fs.initializer) {
        gen_decl(fs.initializer);
    }

    // 2. Collect variables assigned in the loop body AND increment
    Vector<StringView> modified_vars;
    collect_assigned_vars(fs.body, modified_vars);
    collect_assigned_vars_expr(fs.increment, modified_vars);

    // 3. Create blocks
    IRBlock* header_block = create_block("for");
    IRBlock* body_block = create_block("forbody");
    IRBlock* incr_block = create_block("forinc");
    IRBlock* exit_block = create_block("endfor");

    // 4. Create block params on header for modified vars that exist before the loop
    Vector<LoopVarInfo> loop_vars;
    Vector<BlockArgPair> initial_args;
    for (u32 i = 0; i < modified_vars.size(); i++) {
        StringView name = modified_vars[i];
        LocalVar* lv = find_local(name);
        if (lv && lv->value.is_valid()) {
            ValueId param = m_current_func->new_value();
            header_block->params.push_back({param, lv->type, name});
            loop_vars.push_back({name, lv->type, param, lv->value});
            initial_args.push_back({lv->value});
        }
    }

    // 5. Jump to header with initial values
    finish_block_goto(header_block->id, alloc_span(initial_args));

    // 6. In header, bind locals to block params
    set_current_block(header_block);
    for (u32 i = 0; i < loop_vars.size(); i++) {
        define_local(loop_vars[i].name, loop_vars[i].header_param, loop_vars[i].type);
    }

    // 7. Condition and branch
    if (fs.condition) {
        ValueId cond = gen_expr(fs.condition);
        finish_block_branch(cond, body_block->id, exit_block->id);
    } else {
        // No condition = infinite loop (until break)
        finish_block_goto(body_block->id);
    }

    // 8. Push loop info for break/continue
    // continue goes to increment block, but we need to pass args to header after increment
    m_loop_stack.push_back({header_block, exit_block, incr_block, loop_vars});

    // 9. Generate body
    set_current_block(body_block);
    gen_stmt(fs.body);
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        finish_block_goto(incr_block->id);
    }

    // 10. Increment block - generate increment, then jump back to header with args
    set_current_block(incr_block);
    if (fs.increment) {
        gen_expr(fs.increment);
    }
    Span<BlockArgPair> back_args = make_loop_args(m_loop_stack.back().loop_vars);
    finish_block_goto(header_block->id, back_args);

    // Save the loop vars before popping (need them for exit block)
    Vector<LoopVarInfo> saved_loop_vars = m_loop_stack.back().loop_vars;  // copy, not move
    m_loop_stack.pop_back();

    pop_scope();

    // 11. Exit block - use header params as final values
    set_current_block(exit_block);
    for (u32 i = 0; i < saved_loop_vars.size(); i++) {
        define_local(saved_loop_vars[i].name, saved_loop_vars[i].header_param, saved_loop_vars[i].type);
    }
}

void IRBuilder::gen_return_stmt(Stmt* stmt) {
    ReturnStmt& rs = stmt->return_stmt;

    if (rs.value) {
        ValueId val = gen_expr(rs.value);

        // Check if returning a large struct
        if (m_current_func->returns_large_struct()) {
            // Large struct: copy to hidden output pointer (last parameter)
            ValueId output_ptr = m_current_func->params.back().value;
            u32 slot_count = m_current_func->return_type->struct_info.slot_count;
            emit_struct_copy(output_ptr, val, slot_count);
            finish_block_return(ValueId::invalid());  // Return void
        } else {
            finish_block_return(val);
        }
    } else {
        finish_block_return(ValueId::invalid());
    }
}

void IRBuilder::gen_break_stmt(Stmt*) {
    if (m_loop_stack.empty()) return;  // Should be caught by semantic analysis

    LoopInfo& loop = m_loop_stack.back();
    // Exit block doesn't have parameters - it uses header params
    finish_block_goto(loop.exit_block->id);
}

void IRBuilder::gen_continue_stmt(Stmt*) {
    if (m_loop_stack.empty()) return;  // Should be caught by semantic analysis

    LoopInfo& loop = m_loop_stack.back();
    // For while loops: continue_block == header_block, needs args
    // For for loops: continue_block == incr_block, no args needed
    if (loop.continue_block == loop.header_block) {
        // While loop - pass current values to header
        Span<BlockArgPair> args = make_loop_args(loop.loop_vars);
        finish_block_goto(loop.continue_block->id, args);
    } else {
        // For loop - just jump to increment block (no args)
        finish_block_goto(loop.continue_block->id);
    }
}

void IRBuilder::gen_delete_stmt(Stmt* stmt) {
    DeleteStmt& ds = stmt->delete_stmt;
    ValueId val = gen_expr(ds.expr);

    // Get the struct type from the expression
    Type* expr_type = ds.expr->resolved_type;
    Type* struct_type = nullptr;
    if (expr_type && expr_type->kind == TypeKind::Uniq) {
        struct_type = expr_type->ref_info.inner_type;
    }

    // Check if there's a destructor to call
    if (struct_type && struct_type->is_struct()) {
        StructTypeInfo& sti = struct_type->struct_info;

        // Look up destructor by name
        const DestructorInfo* dtor = nullptr;
        for (u32 i = 0; i < sti.destructors.size(); i++) {
            if (sti.destructors[i].name == ds.destructor_name) {
                dtor = &sti.destructors[i];
                break;
            }
        }

        if (dtor) {
            // Call the destructor
            StringView dtor_name = mangle_destructor(sti.name, ds.destructor_name);

            // Build arguments: 'self' pointer + destructor arguments
            Vector<ValueId> call_args;
            call_args.push_back(val);  // 'self' pointer

            for (u32 i = 0; i < ds.arguments.size(); i++) {
                CallArg& arg = ds.arguments[i];
                if (arg.modifier != ParamModifier::None) {
                    // Pass address of lvalue for out/inout args
                    call_args.push_back(gen_lvalue_addr(arg.expr));
                } else {
                    call_args.push_back(gen_expr(arg.expr));
                }
            }

            emit_call(dtor_name, alloc_span(call_args), m_types.void_type());
        } else if (ds.destructor_name.empty()) {
            // Check for default destructor even if not explicitly requested
            for (u32 i = 0; i < sti.destructors.size(); i++) {
                if (sti.destructors[i].name.empty()) {
                    // Found default destructor - call it
                    StringView dtor_name = mangle_destructor(sti.name);
                    Vector<ValueId> call_args;
                    call_args.push_back(val);
                    emit_call(dtor_name, alloc_span(call_args), m_types.void_type());
                    break;
                }
            }
        }
    }

    // Free the memory
    IRInst* inst = emit_inst(IROp::Delete, m_types.void_type());
    if (inst) {
        inst->unary = val;
    }
}

// Expression generation

ValueId IRBuilder::gen_expr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprLiteral:
            return gen_literal_expr(expr);
        case AstKind::ExprIdentifier:
            return gen_identifier_expr(expr);
        case AstKind::ExprUnary:
            return gen_unary_expr(expr);
        case AstKind::ExprBinary:
            return gen_binary_expr(expr);
        case AstKind::ExprTernary:
            return gen_ternary_expr(expr);
        case AstKind::ExprCall:
            return gen_call_expr(expr);
        case AstKind::ExprIndex:
            return gen_index_expr(expr);
        case AstKind::ExprGet:
            return gen_get_expr(expr);
        case AstKind::ExprAssign:
            return gen_assign_expr(expr);
        case AstKind::ExprGrouping:
            return gen_grouping_expr(expr);
        case AstKind::ExprThis:
            return gen_this_expr(expr);
        case AstKind::ExprStructLiteral:
            return gen_struct_literal_expr(expr);
        case AstKind::ExprStaticGet:
            return gen_static_get_expr(expr);
        default:
            return ValueId::invalid();
    }
}

ValueId IRBuilder::gen_literal_expr(Expr* expr) {
    LiteralExpr& lit = expr->literal;

    switch (lit.literal_kind) {
        case LiteralKind::Nil:
            return emit_const_null();
        case LiteralKind::Bool:
            return emit_const_bool(lit.bool_value);
        case LiteralKind::I32:
        case LiteralKind::I64:
        case LiteralKind::U32:
        case LiteralKind::U64:
            return emit_const_int(lit.int_value, expr->resolved_type);
        case LiteralKind::F32:
        case LiteralKind::F64:
            return emit_const_float(lit.float_value, expr->resolved_type);
        case LiteralKind::String:
            return emit_const_string(lit.string_value);
    }
    return ValueId::invalid();
}

ValueId IRBuilder::gen_identifier_expr(Expr* expr) {
    IdentifierExpr& id = expr->identifier;
    ValueId val = lookup_local(id.name);

    // If this is a pointer parameter (out/inout), handle specially
    if (m_param_is_ptr.count(id.name)) {
        Type* type = expr->resolved_type;

        // For struct types, the pointer IS what we need for field access.
        // Don't dereference - struct operations (GET_FIELD, SET_FIELD) expect pointers.
        if (type && type->is_struct()) {
            return val;
        }

        // For primitive types, dereference the pointer to get the value
        u32 slot_count = 1;
        if (type && (type->kind == TypeKind::I64 || type->kind == TypeKind::U64 ||
                    type->kind == TypeKind::F64)) {
            slot_count = 2;
        }
        return emit_load_ptr(val, slot_count, type);
    }

    return val;
}

ValueId IRBuilder::gen_unary_expr(Expr* expr) {
    UnaryExpr& ue = expr->unary;

    ValueId operand = gen_expr(ue.operand);
    Type* result_type = expr->resolved_type;

    IROp op = get_unary_op(ue.op, ue.operand->resolved_type);
    return emit_unary(op, operand, result_type);
}

ValueId IRBuilder::gen_binary_expr(Expr* expr) {
    BinaryExpr& be = expr->binary;

    // Check for string operations - rewrite to native function calls
    Type* left_type = be.left->resolved_type;
    if (left_type && left_type->kind == TypeKind::String) {
        ValueId left = gen_expr(be.left);
        ValueId right = gen_expr(be.right);

        StringView func_name;
        Type* result_type = nullptr;
        i32 native_idx = -1;

        switch (be.op) {
            case BinaryOp::Add:
                func_name = "str_concat";
                result_type = m_types.string_type();
                native_idx = m_registry.get_index(func_name);
                break;
            case BinaryOp::Equal:
                func_name = "str_eq";
                result_type = m_types.bool_type();
                native_idx = m_registry.get_index(func_name);
                break;
            case BinaryOp::NotEqual:
                func_name = "str_ne";
                result_type = m_types.bool_type();
                native_idx = m_registry.get_index(func_name);
                break;
            default:
                // Unsupported string operation - fall through to regular handling
                // (will likely cause a type error)
                break;
        }

        if (native_idx >= 0) {
            Span<ValueId> args = alloc_span<ValueId>(2);
            args[0] = left;
            args[1] = right;
            return emit_call_native(func_name, args, result_type, static_cast<u8>(native_idx));
        }
    }

    // Short-circuit evaluation for && and ||
    if (be.op == BinaryOp::And) {
        ValueId left = gen_expr(be.left);

        IRBlock* right_block = create_block("and.rhs");
        IRBlock* merge_block = create_block("and.end");

        // If left is false, result is false; otherwise evaluate right
        finish_block_branch(left, right_block->id, merge_block->id);

        set_current_block(right_block);
        ValueId right = gen_expr(be.right);
        IRBlock* right_end_block = m_current_block;
        finish_block_goto(merge_block->id);

        // In SSA, we'd need a phi here. For now, use block arguments.
        // Create merge block with a parameter
        set_current_block(merge_block);

        // For simplicity, just return the right value if we got here through right_block
        // This is a simplified approach - proper SSA would need phi/block arguments
        return right;
    }
    else if (be.op == BinaryOp::Or) {
        ValueId left = gen_expr(be.left);

        IRBlock* right_block = create_block("or.rhs");
        IRBlock* merge_block = create_block("or.end");

        // If left is true, result is true; otherwise evaluate right
        finish_block_branch(left, merge_block->id, right_block->id);

        set_current_block(right_block);
        ValueId right = gen_expr(be.right);
        finish_block_goto(merge_block->id);

        set_current_block(merge_block);
        return right;
    }

    // Regular binary operations
    ValueId left = gen_expr(be.left);
    ValueId right = gen_expr(be.right);
    Type* result_type = expr->resolved_type;
    Type* operand_type = be.left->resolved_type;

    // Check if it's a comparison or arithmetic operation
    IROp op;
    switch (be.op) {
        case BinaryOp::Equal:
        case BinaryOp::NotEqual:
        case BinaryOp::Less:
        case BinaryOp::LessEq:
        case BinaryOp::Greater:
        case BinaryOp::GreaterEq:
            op = get_comparison_op(be.op, operand_type);
            break;
        default:
            op = get_binary_op(be.op, operand_type);
            break;
    }

    return emit_binary(op, left, right, result_type);
}

ValueId IRBuilder::gen_ternary_expr(Expr* expr) {
    TernaryExpr& te = expr->ternary;

    ValueId cond = gen_expr(te.condition);

    IRBlock* then_block = create_block("tern.then");
    IRBlock* else_block = create_block("tern.else");
    IRBlock* merge_block = create_block("tern.end");

    finish_block_branch(cond, then_block->id, else_block->id);

    // Then branch
    set_current_block(then_block);
    ValueId then_val = gen_expr(te.then_expr);
    finish_block_goto(merge_block->id);

    // Else branch
    set_current_block(else_block);
    ValueId else_val = gen_expr(te.else_expr);
    finish_block_goto(merge_block->id);

    // Merge - simplified, proper SSA would use block arguments
    set_current_block(merge_block);
    return else_val;  // Simplified
}

ValueId IRBuilder::gen_call_expr(Expr* expr) {
    CallExpr& ce = expr->call;

    // Check if this is a primitive type cast (set by semantic analysis)
    if (ce.callee->kind == AstKind::ExprIdentifier &&
        ce.callee->resolved_type && ce.callee->resolved_type->is_primitive() &&
        !ce.callee->resolved_type->is_void()) {
        return gen_primitive_cast(expr);
    }

    // Check if this is a constructor call - callee has a struct type (set by semantic analysis)
    if (ce.callee->kind == AstKind::ExprIdentifier &&
        ce.callee->resolved_type && ce.callee->resolved_type->is_struct()) {
        return gen_constructor_call(expr);
    }

    // Check if this is a named constructor call: Type.ctor_name(...)
    // The callee is a GetExpr where the object is a type name (not a variable)
    // For named constructors: ge.object is an identifier that resolves to a STRUCT TYPE (not a variable of struct type)
    // This is detected by checking if the identifier matches a type name
    if (ce.callee->kind == AstKind::ExprGet) {
        GetExpr& ge = ce.callee->get;
        if (ge.object->kind == AstKind::ExprIdentifier) {
            // Check if this is a type name (constructor) or a variable (method call)
            StringView name = ge.object->identifier.name;
            Type* named_type = m_types.named_type_by_name(name);
            if (named_type && named_type->is_struct()) {
                // This is a named constructor call: Type.ctor_name(...)
                return gen_constructor_call(expr);
            }
            // Otherwise, it's a method call on a variable - fall through
        }
    }

    // Check if this is a super call: super() / super.ctor_name() / super.method_name()
    if (ce.callee->kind == AstKind::ExprSuper) {
        return gen_super_call(expr);
    }

    // Track which arguments are inout and their addresses, so we can reload after
    struct InoutArg {
        StringView name;
        ValueId addr;
        Type* type;
        u32 slot_count;
    };
    Vector<InoutArg> inout_args;

    // Check if callee returns a large struct
    Type* callee_return_type = expr->resolved_type;
    bool callee_returns_large_struct = callee_return_type &&
        callee_return_type->is_struct() &&
        callee_return_type->struct_info.slot_count > 4;

    // For large struct returns, allocate stack space and prepare output pointer
    ValueId output_ptr = ValueId::invalid();
    if (callee_returns_large_struct) {
        u32 slot_count = callee_return_type->struct_info.slot_count;
        output_ptr = emit_stack_alloc(slot_count, callee_return_type);
    }

    // Evaluate arguments - for out/inout args, pass address instead of value
    Span<ValueId> args = alloc_span<ValueId>(ce.arguments.size());
    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue
            args[i] = gen_lvalue_addr(arg.expr);

            // If this is an identifier and modifier is Inout/Out, track it for reload
            // But only for primitive types - structs are modified in place through the pointer
            if (arg.modifier == ParamModifier::Inout || arg.modifier == ParamModifier::Out) {
                if (arg.expr->kind == AstKind::ExprIdentifier && !m_param_is_ptr.count(arg.expr->identifier.name)) {
                    Type* type = arg.expr->resolved_type;
                    // Skip structs - they're modified in place, no reload needed
                    if (type && type->is_struct()) {
                        continue;
                    }
                    u32 slot_count = 1;
                    if (type && (type->kind == TypeKind::I64 || type->kind == TypeKind::U64 ||
                                type->kind == TypeKind::F64)) {
                        slot_count = 2;
                    }
                    inout_args.push_back({arg.expr->identifier.name, args[i], type, slot_count});
                }
            }
        } else {
            args[i] = gen_expr(arg.expr);
        }
    }

    // For large struct returns, append the output pointer to arguments
    Span<ValueId> final_args;
    if (callee_returns_large_struct) {
        final_args = alloc_span<ValueId>(args.size() + 1);
        for (u32 i = 0; i < args.size(); i++) {
            final_args[i] = args[i];
        }
        final_args[args.size()] = output_ptr;
    } else {
        final_args = args;
    }

    ValueId result;

    // Get function name from callee
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        StringView func_name = ce.callee->identifier.name;
        StringView lookup_name = func_name;

        // Check if this is an imported function (may have alias)
        Symbol* sym = m_symbols.lookup(func_name);
        if (sym && sym->kind == SymbolKind::ImportedFunction) {
            // Use the original function name for native registry lookup
            lookup_name = sym->imported_func.original_name;
        }

        // Check if this is a native function
        i32 native_idx = m_registry.get_index(lookup_name);

        if (native_idx >= 0) {
            result = emit_call_native(lookup_name, final_args, expr->resolved_type, static_cast<u8>(native_idx));
        } else {
            result = emit_call(func_name, final_args, expr->resolved_type);
        }

        // For large struct returns, the result is the output pointer (already allocated)
        if (callee_returns_large_struct) {
            result = output_ptr;
        }
    }
    else if (ce.callee->kind == AstKind::ExprGet) {
        GetExpr& ge = ce.callee->get;

        // Check for module-qualified call: module.function()
        // The object's resolved_type is nullptr for module references
        if (ge.object->kind == AstKind::ExprIdentifier && ge.object->resolved_type == nullptr) {
            // This is a module-qualified call
            StringView module_name = ge.object->identifier.name;
            StringView func_name = ge.name;

            // Look up in native registry - the function name is just the member name
            i32 native_idx = m_registry.get_index(func_name);

            if (native_idx >= 0) {
                result = emit_call_native(func_name, final_args, expr->resolved_type, static_cast<u8>(native_idx));
            } else {
                // Script module call - emit CallExternal
                result = emit_call_external(module_name, func_name, final_args, expr->resolved_type);
            }

            // For large struct returns, the result is the output pointer
            if (callee_returns_large_struct) {
                result = output_ptr;
            }
        } else {
            // Method call: obj.method(args)
            ValueId obj = gen_expr(ge.object);

            // Get the struct type from the object
            Type* obj_type = ge.object->resolved_type;
            Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

            // Look up method in the type hierarchy to find where it's defined
            // This ensures we use the correct struct name for mangling (important for inheritance)
            Type* method_owner = nullptr;
            StringView method_name = ge.name;
            if (struct_type && struct_type->is_struct()) {
                lookup_method_in_hierarchy(struct_type, ge.name, &method_owner);
                Type* name_type = method_owner ? method_owner : struct_type;
                method_name = mangle_method(name_type->struct_info.name, ge.name);
            }

            // Prepend object to arguments, append output pointer if large struct return
            Span<ValueId> method_args;
            if (callee_returns_large_struct) {
                // obj + args + output_ptr
                method_args = alloc_span<ValueId>(args.size() + 2);
                method_args[0] = obj;
                for (u32 i = 0; i < args.size(); i++) {
                    method_args[i + 1] = args[i];
                }
                method_args[args.size() + 1] = output_ptr;
            } else {
                method_args = alloc_span<ValueId>(args.size() + 1);
                method_args[0] = obj;
                for (u32 i = 0; i < args.size(); i++) {
                    method_args[i + 1] = args[i];
                }
            }

            result = emit_call(method_name, method_args, expr->resolved_type);

            // For large struct returns, the result is the output pointer
            if (callee_returns_large_struct) {
                result = output_ptr;
            }
        }
    }
    else {
        return ValueId::invalid();
    }

    // After the call, reload inout variables from their stack addresses
    for (const InoutArg& ia : inout_args) {
        ValueId new_val = emit_load_ptr(ia.addr, ia.slot_count, ia.type);
        define_local(ia.name, new_val, ia.type);
    }

    return result;
}

ValueId IRBuilder::gen_index_expr(Expr* expr) {
    IndexExpr& ie = expr->index;

    ValueId obj = gen_expr(ie.object);
    ValueId index = gen_expr(ie.index);

    return emit_get_index(obj, index, expr->resolved_type);
}

ValueId IRBuilder::gen_get_expr(Expr* expr) {
    GetExpr& ge = expr->get;

    ValueId obj = gen_expr(ge.object);

    // Get the struct type from the object
    Type* obj_type = ge.object->resolved_type;
    Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

    u32 slot_offset = 0;
    u32 slot_count = 1;
    Type* field_type = nullptr;
    if (struct_type && struct_type->is_struct()) {
        const FieldInfo* fi = struct_type->struct_info.find_field(ge.name);
        if (fi) {
            slot_offset = fi->slot_offset;
            slot_count = fi->slot_count;
            field_type = fi->type;
        }
    }

    // If the field is a struct type, we need to compute its address (pointer)
    // instead of loading its value, since nested struct access needs the address.
    if (field_type && field_type->is_struct()) {
        return emit_get_field_addr(obj, ge.name, slot_offset, expr->resolved_type);
    }

    return emit_get_field(obj, ge.name, slot_offset, slot_count, expr->resolved_type);
}

ValueId IRBuilder::gen_assign_expr(Expr* expr) {
    AssignExpr& ae = expr->assign;

    // Get the value to assign
    ValueId value = gen_expr(ae.value);

    // Handle compound assignment
    if (ae.op != AssignOp::Assign) {
        ValueId old_val = gen_expr(ae.target);
        Type* type = ae.target->resolved_type;

        IROp op;
        switch (ae.op) {
            case AssignOp::AddAssign:
                op = type->is_float() ? IROp::AddF : IROp::AddI;
                break;
            case AssignOp::SubAssign:
                op = type->is_float() ? IROp::SubF : IROp::SubI;
                break;
            case AssignOp::MulAssign:
                op = type->is_float() ? IROp::MulF : IROp::MulI;
                break;
            case AssignOp::DivAssign:
                op = type->is_float() ? IROp::DivF : IROp::DivI;
                break;
            case AssignOp::ModAssign:
                op = IROp::ModI;
                break;
            default:
                op = IROp::Copy;
                break;
        }

        value = emit_binary(op, old_val, value, type);
    }

    // Assign based on target type
    if (ae.target->kind == AstKind::ExprIdentifier) {
        StringView name = ae.target->identifier.name;

        // If this is a pointer parameter (out/inout), store through the pointer
        if (m_param_is_ptr.count(name)) {
            ValueId ptr = lookup_local(name);
            Type* type = expr->resolved_type;
            u32 slot_count = 1;
            if (type && type->is_struct()) {
                slot_count = type->struct_info.slot_count;
            } else if (type && (type->kind == TypeKind::I64 || type->kind == TypeKind::U64 ||
                               type->kind == TypeKind::F64)) {
                slot_count = 2;
            }
            return emit_store_ptr(ptr, value, slot_count, type);
        }

        // Normal variable assignment - in SSA, we create a new value
        define_local(name, value, expr->resolved_type);
        return value;
    }
    else if (ae.target->kind == AstKind::ExprGet) {
        // Field assignment
        GetExpr& ge = ae.target->get;
        ValueId obj = gen_expr(ge.object);

        // Get the struct type from the object
        Type* obj_type = ge.object->resolved_type;
        Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

        u32 slot_offset = 0;
        u32 slot_count = 1;
        if (struct_type && struct_type->is_struct()) {
            const FieldInfo* fi = struct_type->struct_info.find_field(ge.name);
            if (fi) {
                slot_offset = fi->slot_offset;
                slot_count = fi->slot_count;
            }
        }

        return emit_set_field(obj, ge.name, slot_offset, slot_count, value, expr->resolved_type);
    }
    else if (ae.target->kind == AstKind::ExprIndex) {
        // Index assignment
        IndexExpr& ie = ae.target->index;
        ValueId obj = gen_expr(ie.object);
        ValueId index = gen_expr(ie.index);
        return emit_set_index(obj, index, value, expr->resolved_type);
    }

    return value;
}

ValueId IRBuilder::gen_grouping_expr(Expr* expr) {
    return gen_expr(expr->grouping.expr);
}

ValueId IRBuilder::gen_this_expr(Expr* expr) {
    // 'self' is the first parameter in methods
    return lookup_local("self");
}

ValueId IRBuilder::gen_primitive_cast(Expr* expr) {
    CallExpr& ce = expr->call;

    // Get target type from callee (set by semantic analysis)
    Type* target_type = ce.callee->resolved_type;

    // Get source value and type
    ValueId source = gen_expr(ce.arguments[0].expr);
    Type* source_type = ce.arguments[0].expr->resolved_type;

    // If same type, no-op
    if (source_type == target_type) {
        return source;
    }

    // Emit Cast instruction
    IRInst* inst = emit_inst(IROp::Cast, target_type);
    inst->cast.source = source;
    inst->cast.source_type = source_type;
    return inst->result;
}

ValueId IRBuilder::gen_constructor_call(Expr* expr) {
    CallExpr& ce = expr->call;
    Type* result_type = expr->resolved_type;

    // Get struct type from callee (set by semantic analysis)
    // For Type(), callee is an identifier with struct type
    // For Type.ctor_name(), callee is a GetExpr with object having struct type
    Type* struct_type = nullptr;
    if (ce.callee->kind == AstKind::ExprIdentifier) {
        struct_type = ce.callee->resolved_type;
    } else if (ce.callee->kind == AstKind::ExprGet) {
        struct_type = ce.callee->get.object->resolved_type;
    }

    // Determine allocation mode and final struct type
    ValueId obj;
    if (ce.is_heap) {
        // uniq Type() - heap allocation
        // result_type is uniq<StructType>
        Span<ValueId> empty_args = {};
        obj = emit_new(struct_type->struct_info.name, empty_args, result_type);
    } else {
        // Type() - stack allocation
        // result_type is StructType (value type)
        u32 slot_count = struct_type->struct_info.slot_count;
        obj = emit_stack_alloc(slot_count, struct_type);
    }

    // Call constructor (user-defined or synthesized)
    StructTypeInfo& sti = struct_type->struct_info;
    StringView ctor_name = mangle_constructor(sti.name, ce.constructor_name);

    // Build arguments: 'self' pointer + constructor arguments
    Vector<ValueId> call_args;
    call_args.push_back(obj);  // 'self' pointer

    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            // Pass address of lvalue for out/inout args
            call_args.push_back(gen_lvalue_addr(arg.expr));
        } else {
            call_args.push_back(gen_expr(arg.expr));
        }
    }

    emit_call(ctor_name, alloc_span(call_args), m_types.void_type());

    return obj;
}

ValueId IRBuilder::gen_super_call(Expr* expr) {
    CallExpr& ce = expr->call;
    SuperExpr& se = ce.callee->super_expr;

    // Get the 'self' pointer
    ValueId self_ptr = lookup_local("self");

    // Get the parent struct type from the callee's resolved_type (ref<StructType>)
    Type* ref_type = ce.callee->resolved_type;
    Type* target_type = nullptr;
    if (ref_type && ref_type->is_reference()) {
        target_type = ref_type->ref_info.inner_type;
    }

    if (!target_type || !target_type->is_struct()) {
        return ValueId::invalid();
    }

    // Determine if this is a constructor call or method call
    // Constructor calls return void (expr->resolved_type is void)
    // Method calls return the method's return type
    bool is_constructor_call = expr->resolved_type && expr->resolved_type->kind == TypeKind::Void;

    StructTypeInfo& target_sti = target_type->struct_info;

    StringView call_name;
    if (is_constructor_call) {
        call_name = mangle_constructor(target_sti.name, se.method_name);
    } else {
        call_name = mangle_method(target_sti.name, se.method_name);
    }

    // Evaluate arguments
    Span<ValueId> args = alloc_span<ValueId>(ce.arguments.size());
    for (u32 i = 0; i < ce.arguments.size(); i++) {
        CallArg& arg = ce.arguments[i];
        if (arg.modifier != ParamModifier::None) {
            args[i] = gen_lvalue_addr(arg.expr);
        } else {
            args[i] = gen_expr(arg.expr);
        }
    }

    // Prepend self to arguments
    Span<ValueId> call_args = alloc_span<ValueId>(args.size() + 1);
    call_args[0] = self_ptr;
    for (u32 i = 0; i < args.size(); i++) {
        call_args[i + 1] = args[i];
    }

    return emit_call(call_name, call_args, expr->resolved_type);
}

ValueId IRBuilder::gen_struct_literal_expr(Expr* expr) {
    StructLiteralExpr& sl = expr->struct_literal;
    Type* result_type = expr->resolved_type;

    // Determine struct type and allocation mode
    Type* struct_type;
    ValueId struct_ptr;

    if (sl.is_heap) {
        // uniq Type { ... } - heap allocation
        struct_type = result_type->ref_info.inner_type;
        Span<ValueId> empty_args = {};
        struct_ptr = emit_new(sl.type_name, empty_args, result_type);
    } else {
        // Type { ... } - stack allocation
        struct_type = result_type;
        u32 slot_count = struct_type->struct_info.slot_count;
        struct_ptr = emit_stack_alloc(slot_count, struct_type);
    }

    // Build map of provided field initializers
    tsl::robin_map<StringView, Expr*, StringViewHash, StringViewEqual> provided_fields;
    for (u32 i = 0; i < sl.fields.size(); i++) {
        provided_fields[sl.fields[i].name] = sl.fields[i].value;
    }

    // Helper to find default value for a field by searching the inheritance chain
    auto find_field_default = [](Type* type, StringView field_name) -> Expr* {
        Type* current = type;
        while (current && current->is_struct()) {
            StructDecl& sd = current->struct_info.decl->struct_decl;
            for (u32 j = 0; j < sd.fields.size(); j++) {
                if (sd.fields[j].name == field_name) {
                    return sd.fields[j].default_value;
                }
            }
            current = current->struct_info.parent;
        }
        return nullptr;
    };

    // Initialize ALL fields (provided or default)
    for (u32 i = 0; i < struct_type->struct_info.fields.size(); i++) {
        FieldInfo& fi = struct_type->struct_info.fields[i];
        Expr* value_expr = nullptr;

        auto it = provided_fields.find(fi.name);
        if (it != provided_fields.end()) {
            value_expr = it->second;
        } else {
            // Use default value from struct declaration (searching inheritance chain)
            value_expr = find_field_default(struct_type, fi.name);
        }

        ValueId value = gen_expr(value_expr);

        // For struct-typed fields, use StructCopy since the value is a pointer
        if (fi.type && fi.type->is_struct()) {
            // Get address of the field
            ValueId field_addr = emit_get_field_addr(struct_ptr, fi.name, fi.slot_offset, fi.type);
            // Copy struct data from value (source pointer) to field_addr (dest pointer)
            emit_struct_copy(field_addr, value, fi.slot_count);
        } else {
            emit_set_field(struct_ptr, fi.name, fi.slot_offset, fi.slot_count, value, fi.type);
        }
    }

    return struct_ptr;
}

ValueId IRBuilder::gen_static_get_expr(Expr* expr) {
    StaticGetExpr& sge = expr->static_get;

    // Currently only enum variants use static get (Type::Variant)
    // Look up the enum variant symbol to get its value
    Symbol* sym = m_symbols.lookup(sge.member_name);
    if (sym && sym->kind == SymbolKind::EnumVariant) {
        // Emit the enum variant's integer value
        return emit_const_int(sym->enum_variant.value, expr->resolved_type);
    }

    // Should not reach here if semantic analysis passed
    return ValueId::invalid();
}

ValueId IRBuilder::gen_lvalue_addr(Expr* expr) {
    if (!expr) return ValueId::invalid();

    switch (expr->kind) {
        case AstKind::ExprIdentifier: {
            StringView name = expr->identifier.name;
            // If this is already a pointer parameter, return its value directly
            if (m_param_is_ptr.count(name)) {
                return lookup_local(name);
            }

            ValueId current_val = lookup_local(name);
            Type* type = expr->resolved_type;

            // For struct types, the variable is already a pointer to stack-allocated data.
            // Just return the existing pointer - no copy needed.
            if (type && type->is_struct()) {
                return current_val;
            }

            // For primitive types, we need to:
            // 1. Allocate a stack slot
            // 2. Store the current value to the slot
            // 3. Return the address

            // Calculate slot count
            u32 slot_count = 1;
            if (type && (type->kind == TypeKind::I64 || type->kind == TypeKind::U64 ||
                        type->kind == TypeKind::F64)) {
                slot_count = 2;
            }

            // Allocate stack space
            ValueId addr = emit_stack_alloc(slot_count, type);

            // Store current value to the stack slot (using SetField with offset 0)
            emit_set_field(addr, name, 0, slot_count, current_val, type);

            return addr;
        }
        case AstKind::ExprGet: {
            // Field access - use GET_FIELD_ADDR
            GetExpr& ge = expr->get;
            ValueId obj = gen_expr(ge.object);

            // Get the struct type from the object
            Type* obj_type = ge.object->resolved_type;
            Type* struct_type = obj_type ? obj_type->base_type() : nullptr;

            u32 slot_offset = 0;
            if (struct_type && struct_type->is_struct()) {
                const FieldInfo* fi = struct_type->struct_info.find_field(ge.name);
                if (fi) {
                    slot_offset = fi->slot_offset;
                }
            }

            return emit_get_field_addr(obj, ge.name, slot_offset, expr->resolved_type);
        }
        case AstKind::ExprGrouping:
            return gen_lvalue_addr(expr->grouping.expr);
        default:
            // Should not happen - semantic analysis validated lvalues
            return ValueId::invalid();
    }
}

// Declaration generation

void IRBuilder::gen_decl(Decl* decl) {
    if (!decl) return;

    switch (decl->kind) {
        case AstKind::DeclVar:
            gen_var_decl(decl);
            break;
        default:
            // Statement wrapped in declaration
            if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtDelete) {
                gen_stmt(&decl->stmt);
            }
            break;
    }
}

void IRBuilder::gen_var_decl(Decl* decl) {
    VarDecl& vd = decl->var_decl;

    // Use the resolved type from semantic analysis
    Type* type = vd.resolved_type;
    if (!type) {
        type = m_types.error_type();
    }

    ValueId value;

    // Check if this is a struct type - needs stack allocation
    if (type->is_struct()) {
        // If the initializer is a struct literal, it already allocates and initializes
        if (vd.initializer && vd.initializer->kind == AstKind::ExprStructLiteral) {
            value = gen_expr(vd.initializer);
        } else if (vd.initializer) {
            // Initializer is an expression returning a struct (e.g., function call)
            // The call returns a pointer to struct data that we need to copy
            value = gen_expr(vd.initializer);
            // Note: The lowering handles struct return unpacking, so value is already
            // a pointer to the struct data on the local stack
        } else {
            // No initializer - allocate stack space for the struct (zero-initialized by VM)
            u32 slot_count = type->struct_info.slot_count;
            value = emit_stack_alloc(slot_count, type);
        }
    } else {
        // Non-struct types: use register storage
        if (vd.initializer) {
            value = gen_expr(vd.initializer);
        } else {
            // Default initialization
            value = emit_const_null();
        }
    }

    define_local(vd.name, value, type);
}

// Variable management

void IRBuilder::define_local(StringView name, ValueId value, Type* type) {
    if (m_local_scopes.empty()) return;

    // Search for an existing binding in outer scopes and update it
    // This is necessary for SSA - assignments should update the existing definition
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            m_local_scopes[i][name] = {value, type};
            return;
        }
    }

    // If no existing binding, add to innermost scope (new variable declaration)
    m_local_scopes.back()[name] = {value, type};
}

ValueId IRBuilder::lookup_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return it->second.value;
        }
    }
    return ValueId::invalid();
}

void IRBuilder::push_scope() {
    m_local_scopes.push_back({});
}

void IRBuilder::pop_scope() {
    if (!m_local_scopes.empty()) {
        m_local_scopes.pop_back();
    }
}

IRBuilder::LocalVar* IRBuilder::find_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return &it.value();
        }
    }
    return nullptr;
}

void IRBuilder::collect_assigned_vars(Stmt* stmt, Vector<StringView>& out) {
    if (!stmt) return;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            collect_assigned_vars_expr(stmt->expr_stmt.expr, out);
            break;
        case AstKind::StmtBlock: {
            BlockStmt& block = stmt->block;
            for (u32 i = 0; i < block.declarations.size(); i++) {
                Decl* d = block.declarations[i];
                if (!d) continue;
                // Recurse into statements (not var decls - those are new vars)
                if (d->kind >= AstKind::StmtExpr && d->kind <= AstKind::StmtDelete) {
                    collect_assigned_vars(&d->stmt, out);
                }
            }
            break;
        }
        case AstKind::StmtIf:
            collect_assigned_vars(stmt->if_stmt.then_branch, out);
            collect_assigned_vars(stmt->if_stmt.else_branch, out);
            break;
        case AstKind::StmtWhile:
            collect_assigned_vars(stmt->while_stmt.body, out);
            break;
        case AstKind::StmtFor:
            collect_assigned_vars(stmt->for_stmt.body, out);
            collect_assigned_vars_expr(stmt->for_stmt.increment, out);
            break;
        default:
            break;
    }
}

void IRBuilder::collect_assigned_vars_expr(Expr* expr, Vector<StringView>& out) {
    if (!expr) return;

    switch (expr->kind) {
        case AstKind::ExprAssign: {
            // Check if the target is an identifier
            if (expr->assign.target->kind == AstKind::ExprIdentifier) {
                StringView name = expr->assign.target->identifier.name;
                // Add if not already present
                bool found = false;
                for (u32 i = 0; i < out.size(); i++) {
                    if (out[i] == name) {
                        found = true;
                        break;
                    }
                }
                if (!found) {
                    out.push_back(name);
                }
            }
            // Recurse into value expression (it might have nested assignments)
            collect_assigned_vars_expr(expr->assign.value, out);
            break;
        }
        case AstKind::ExprBinary:
            collect_assigned_vars_expr(expr->binary.left, out);
            collect_assigned_vars_expr(expr->binary.right, out);
            break;
        case AstKind::ExprUnary:
            collect_assigned_vars_expr(expr->unary.operand, out);
            break;
        case AstKind::ExprTernary:
            collect_assigned_vars_expr(expr->ternary.condition, out);
            collect_assigned_vars_expr(expr->ternary.then_expr, out);
            collect_assigned_vars_expr(expr->ternary.else_expr, out);
            break;
        case AstKind::ExprCall:
            collect_assigned_vars_expr(expr->call.callee, out);
            for (u32 i = 0; i < expr->call.arguments.size(); i++) {
                collect_assigned_vars_expr(expr->call.arguments[i].expr, out);
            }
            break;
        case AstKind::ExprIndex:
            collect_assigned_vars_expr(expr->index.object, out);
            collect_assigned_vars_expr(expr->index.index, out);
            break;
        case AstKind::ExprGet:
            collect_assigned_vars_expr(expr->get.object, out);
            break;
        case AstKind::ExprGrouping:
            collect_assigned_vars_expr(expr->grouping.expr, out);
            break;
        default:
            break;
    }
}

Span<BlockArgPair> IRBuilder::make_loop_args(const Vector<LoopVarInfo>& loop_vars) {
    if (loop_vars.empty()) return {};

    Vector<BlockArgPair> args;
    for (u32 i = 0; i < loop_vars.size(); i++) {
        // Look up the current value of this variable
        ValueId current_val = lookup_local(loop_vars[i].name);
        args.push_back({current_val});
    }
    return alloc_span(args);
}

// Opcode selection

IROp IRBuilder::get_binary_op(BinaryOp op, Type* type) {
    bool is_float = type && type->is_float();

    switch (op) {
        case BinaryOp::Add:
            return is_float ? IROp::AddF : IROp::AddI;
        case BinaryOp::Sub:
            return is_float ? IROp::SubF : IROp::SubI;
        case BinaryOp::Mul:
            return is_float ? IROp::MulF : IROp::MulI;
        case BinaryOp::Div:
            return is_float ? IROp::DivF : IROp::DivI;
        case BinaryOp::Mod:
            return IROp::ModI;
        case BinaryOp::BitAnd:
            return IROp::BitAnd;
        case BinaryOp::BitOr:
            return IROp::BitOr;
        default:
            return IROp::Copy;
    }
}

IROp IRBuilder::get_comparison_op(BinaryOp op, Type* type) {
    bool is_float = type && type->is_float();

    switch (op) {
        case BinaryOp::Equal:
            return is_float ? IROp::EqF : IROp::EqI;
        case BinaryOp::NotEqual:
            return is_float ? IROp::NeF : IROp::NeI;
        case BinaryOp::Less:
            return is_float ? IROp::LtF : IROp::LtI;
        case BinaryOp::LessEq:
            return is_float ? IROp::LeF : IROp::LeI;
        case BinaryOp::Greater:
            return is_float ? IROp::GtF : IROp::GtI;
        case BinaryOp::GreaterEq:
            return is_float ? IROp::GeF : IROp::GeI;
        default:
            return IROp::EqI;
    }
}

IROp IRBuilder::get_unary_op(UnaryOp op, Type* type) {
    switch (op) {
        case UnaryOp::Negate:
            return (type && type->is_float()) ? IROp::NegF : IROp::NegI;
        case UnaryOp::Not:
            return IROp::Not;
        case UnaryOp::BitNot:
            return IROp::BitNot;
    }
    return IROp::Copy;
}

// Name mangling helpers

StringView IRBuilder::mangle_method(StringView struct_name, StringView method_name) {
    char name_buf[256];
    snprintf(name_buf, sizeof(name_buf), "%.*s$$%.*s",
             static_cast<int>(struct_name.size()), struct_name.data(),
             static_cast<int>(method_name.size()), method_name.data());
    u32 len = static_cast<u32>(strlen(name_buf));
    char* name_ptr = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
    memcpy(name_ptr, name_buf, len + 1);
    return StringView(name_ptr, len);
}

StringView IRBuilder::mangle_constructor(StringView struct_name, StringView ctor_name) {
    char name_buf[256];
    if (ctor_name.empty()) {
        snprintf(name_buf, sizeof(name_buf), "%.*s$$new",
                 static_cast<int>(struct_name.size()), struct_name.data());
    } else {
        snprintf(name_buf, sizeof(name_buf), "%.*s$$new$$%.*s",
                 static_cast<int>(struct_name.size()), struct_name.data(),
                 static_cast<int>(ctor_name.size()), ctor_name.data());
    }
    u32 len = static_cast<u32>(strlen(name_buf));
    char* name_ptr = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
    memcpy(name_ptr, name_buf, len + 1);
    return StringView(name_ptr, len);
}

Type* IRBuilder::apply_ref_kind(Type* base_type, RefKind ref_kind) {
    switch (ref_kind) {
        case RefKind::Uniq: return m_types.uniq_type(base_type);
        case RefKind::Ref:  return m_types.ref_type(base_type);
        case RefKind::Weak: return m_types.weak_type(base_type);
        case RefKind::None: return base_type;
    }
    return base_type;
}

void IRBuilder::begin_function_body(bool skip_hidden_return) {
    // Create entry block
    IRBlock* entry = create_block("entry");
    set_current_block(entry);

    // Initialize local variable scopes
    m_local_scopes.clear();
    push_scope();

    // Add function parameters to local scope
    // Skip the hidden return pointer parameter if requested
    u32 param_count = skip_hidden_return && m_current_func->returns_large_struct()
        ? m_current_func->params.size() - 1
        : m_current_func->params.size();
    for (u32 i = 0; i < param_count; i++) {
        BlockParam& bp = m_current_func->params[i];
        define_local(bp.name, bp.value, bp.type);
    }

    // Emit RefInc for ref-typed parameters at function entry
    // This tracks borrows in the constraint reference model
    for (ValueId ref_param : m_ref_params) {
        emit_ref_inc(ref_param);
    }
}

void IRBuilder::end_function_body() {
    // If current block doesn't have a terminator, add implicit return
    if (m_current_block && m_current_block->terminator.kind == TerminatorKind::None) {
        if (m_current_func->return_type->is_void()) {
            finish_block_return(ValueId::invalid());
        } else {
            // This shouldn't happen if semantic analysis passed
            // Return a default value
            ValueId default_val = emit_const_null();
            finish_block_return(default_val);
        }
    }

    pop_scope();
}

void IRBuilder::setup_parameters(Span<Param> params, Type* self_type) {
    // Clear parameter tracking
    m_param_is_ptr.clear();
    m_ref_params.clear();

    // Add 'self' parameter if this is a method/constructor/destructor
    if (self_type) {
        BlockParam self_param;
        self_param.value = m_current_func->new_value();
        self_param.type = m_types.ref_type(self_type);
        self_param.name = "self";
        m_current_func->params.push_back(self_param);
        m_current_func->param_is_ptr.push_back(true);
        m_param_is_ptr["self"] = true;
    }

    // Set up other parameters
    for (u32 i = 0; i < params.size(); i++) {
        Param& param = params[i];
        Type* param_type = nullptr;
        if (param.type) {
            param_type = m_types.type_by_name(param.type->name);
            if (!param_type) {
                param_type = m_types.error_type();
            }
            param_type = apply_ref_kind(param_type, param.type->ref_kind);
        }

        bool is_ptr = (param.modifier != ParamModifier::None);
        if (is_ptr) {
            m_param_is_ptr[param.name] = true;
        }

        BlockParam bp;
        bp.value = m_current_func->new_value();
        bp.type = param_type;
        bp.name = param.name;
        m_current_func->params.push_back(bp);
        m_current_func->param_is_ptr.push_back(is_ptr);

        // Track ref-typed parameters for RefInc/RefDec at boundaries
        if (param_type && param_type->kind == TypeKind::Ref) {
            m_ref_params.push_back(bp.value);
        }
    }
}

StringView IRBuilder::mangle_destructor(StringView struct_name, StringView dtor_name) {
    char name_buf[256];
    if (dtor_name.empty()) {
        snprintf(name_buf, sizeof(name_buf), "%.*s$$delete",
                 static_cast<int>(struct_name.size()), struct_name.data());
    } else {
        snprintf(name_buf, sizeof(name_buf), "%.*s$$delete$$%.*s",
                 static_cast<int>(struct_name.size()), struct_name.data(),
                 static_cast<int>(dtor_name.size()), dtor_name.data());
    }
    u32 len = static_cast<u32>(strlen(name_buf));
    char* name_ptr = reinterpret_cast<char*>(m_allocator.alloc_bytes(len + 1, 1));
    memcpy(name_ptr, name_buf, len + 1);
    return StringView(name_ptr, len);
}

}

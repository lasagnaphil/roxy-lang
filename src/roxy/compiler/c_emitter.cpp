#include "roxy/compiler/c_emitter.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/core/format.hpp"

namespace rx {

CEmitter::CEmitter(BumpAllocator& alloc, const CEmitterConfig& config)
    : m_config(config), m_alloc(alloc) {}

// --- Type emission ---

void CEmitter::emit_type(Type* type, String& out) {
    if (!type) {
        out.append("void");
        return;
    }
    switch (type->kind) {
        case TypeKind::Void:   out.append("void");     break;
        case TypeKind::Bool:   out.append("bool");      break;
        case TypeKind::I8:     out.append("int8_t");    break;
        case TypeKind::I16:    out.append("int16_t");   break;
        case TypeKind::I32:    out.append("int32_t");   break;
        case TypeKind::I64:    out.append("int64_t");   break;
        case TypeKind::U8:     out.append("uint8_t");   break;
        case TypeKind::U16:    out.append("uint16_t");  break;
        case TypeKind::U32:    out.append("uint32_t");  break;
        case TypeKind::U64:    out.append("uint64_t");  break;
        case TypeKind::F32:    out.append("float");     break;
        case TypeKind::F64:    out.append("double");    break;
        case TypeKind::Struct:
            emit_mangled_name(type->struct_info.name, out);
            break;
        case TypeKind::Enum:
            emit_mangled_name(type->enum_info.name, out);
            break;
        case TypeKind::Ref:
        case TypeKind::Uniq:
            emit_type(type->ref_info.inner_type, out);
            out.append("*");
            break;
        case TypeKind::String:
            out.append("void*");
            break;
        case TypeKind::List:
            out.append("void*");
            break;
        case TypeKind::Map:
            out.append("void*");
            break;
        case TypeKind::Weak:
            out.append("roxy_weak");
            break;
        case TypeKind::Nil:
            out.append("void*");
            break;
        default:
            out.append("/* unsupported type */ void");
            break;
    }
}

// --- Name mangling: $$ -> __, $ -> _ ---

void CEmitter::emit_mangled_name(StringView name, String& out) {
    const char* data = name.data();
    u32 len = name.size();
    for (u32 i = 0; i < len; i++) {
        if (data[i] == '$' && i + 1 < len && data[i + 1] == '$') {
            out.append("__");
            i++; // skip second $
        } else if (data[i] == '$') {
            out.push_back('_');
        } else {
            out.push_back(data[i]);
        }
    }
}

// --- Value helpers ---

void CEmitter::emit_value(ValueId id, String& out) {
    char buf[16];
    format_to(buf, sizeof(buf), "v{}", id.id);
    out.append(buf);
}

Type* CEmitter::get_value_type(ValueId id) {
    auto it = m_value_types.find(id.id);
    if (it != m_value_types.end()) return it->second;
    return nullptr;
}

bool CEmitter::is_stack_alloc_value(ValueId id) {
    return m_stack_alloc_values.find(id.id) != m_stack_alloc_values.end();
}

bool CEmitter::is_pointer_value(ValueId id) {
    return m_pointer_values.find(id.id) != m_pointer_values.end();
}

void CEmitter::emit_field_access(ValueId object, StringView field_name, String& out) {
    emit_value(object, out);
    if (is_pointer_value(object)) {
        out.append("->");
    } else {
        out.push_back('.');
    }
    out.append(field_name.data(), field_name.size());
}

const IRFunction* CEmitter::find_function(StringView name) {
    if (!m_module) return nullptr;
    for (u32 i = 0; i < m_module->functions.size(); i++) {
        if (m_module->functions[i]->name == name) {
            return m_module->functions[i];
        }
    }
    return nullptr;
}

bool CEmitter::is_scalar_stack_alloc(ValueId id) {
    if (!is_stack_alloc_value(id)) return false;
    Type* val_type = get_value_type(id);
    return val_type && !val_type->is_struct();
}

// --- Collect value types ---

void CEmitter::collect_value_types(const IRFunction* func) {
    m_value_types.clear();
    m_stack_alloc_values.clear();
    m_pointer_values.clear();
    m_var_name_to_value.clear();
    m_const_int_values.clear();

    for (u32 i = 0; i < func->params.size(); i++) {
        m_value_types[func->params[i].value.id] = func->params[i].type;
        // Struct params and ref/uniq params are pointer values in the C output
        Type* param_type = func->params[i].type;
        if (param_type && (param_type->is_struct() || param_type->is_reference())) {
            m_pointer_values.insert(func->params[i].value.id);
        }
        // out/inout params are also pointers
        if (i < func->param_is_ptr.size() && func->param_is_ptr[i]) {
            m_pointer_values.insert(func->params[i].value.id);
        }
        if (func->params[i].name.data() && func->params[i].name.size() > 0) {
            u32 name_hash = 0;
            for (u32 c = 0; c < func->params[i].name.size(); c++) {
                name_hash = name_hash * 31 + func->params[i].name.data()[c];
            }
            m_var_name_to_value[name_hash] = func->params[i].value;
        }
    }

    for (u32 b = 0; b < func->blocks.size(); b++) {
        const IRBlock* block = func->blocks[b];
        for (u32 p = 0; p < block->params.size(); p++) {
            m_value_types[block->params[p].value.id] = block->params[p].type;
            if (block->params[p].name.data() && block->params[p].name.size() > 0) {
                u32 name_hash = 0;
                for (u32 c = 0; c < block->params[p].name.size(); c++) {
                    name_hash = name_hash * 31 + block->params[p].name.data()[c];
                }
                m_var_name_to_value[name_hash] = block->params[p].value;
            }
        }
        for (u32 i = 0; i < block->instructions.size(); i++) {
            const IRInst* inst = block->instructions[i];
            if (inst->result.is_valid() && inst->type) {
                m_value_types[inst->result.id] = inst->type;
            }
            if (inst->op == IROp::ConstInt) {
                m_const_int_values[inst->result.id] = inst->const_data.int_val;
            }
            if (inst->op == IROp::StackAlloc) {
                m_stack_alloc_values.insert(inst->result.id);
                m_pointer_values.insert(inst->result.id);
            }
            if (inst->op == IROp::GetFieldAddr) {
                m_pointer_values.insert(inst->result.id);
            }
            if (inst->op == IROp::New) {
                m_pointer_values.insert(inst->result.id);
            }
            // Container reads with struct value type return a pointer into
            // the container's backing storage (matches Roxy's "all struct
            // rvalues are pointers" convention). Tracked as pointer values
            // so the local declaration emits `StructType* vN;`.
            if (inst->op == IROp::IndexGet && inst->type && inst->type->is_struct()) {
                m_pointer_values.insert(inst->result.id);
            }
            if (inst->op == IROp::CallNative && inst->type && inst->type->is_struct()) {
                StringView fn = inst->call.func_name;
                // The IR-side names for getter natives end in $$get / $$index
                // / $$pop (post-mangling). Match by suffix.
                auto ends_with = [](StringView s, const char* suffix) {
                    u32 sl = static_cast<u32>(strlen(suffix));
                    return s.size() >= sl &&
                        memcmp(s.data() + s.size() - sl, suffix, sl) == 0;
                };
                if (ends_with(fn, "$$get") || ends_with(fn, "$$index") ||
                    ends_with(fn, "$$pop")) {
                    m_pointer_values.insert(inst->result.id);
                }
            }
        }
    }
}

// --- Enum typedefs ---

void CEmitter::emit_enum_typedefs(const IRModule* module, String& out) {
    for (u32 e = 0; e < module->enum_types.size(); e++) {
        Type* enum_type = module->enum_types[e];
        const EnumTypeInfo& info = enum_type->enum_info;

        out.append("typedef enum { ");

        // Get variants from the AST decl
        if (info.decl) {
            const EnumDecl& enum_decl = info.decl->enum_decl;
            i64 next_value = 0;
            for (u32 v = 0; v < enum_decl.variants.size(); v++) {
                if (v > 0) out.append(", ");
                emit_mangled_name(info.name, out);
                out.push_back('_');
                out.append(enum_decl.variants[v].name.data(), enum_decl.variants[v].name.size());

                i64 value = next_value;
                if (enum_decl.variants[v].value) {
                    // Explicit value from literal expression
                    value = enum_decl.variants[v].value->literal.int_value;
                }
                char buf[32];
                format_to(buf, sizeof(buf), " = {}", value);
                out.append(buf);
                next_value = value + 1;
            }
        }

        out.append(" } ");
        emit_mangled_name(info.name, out);
        out.append(";\n");
    }
    if (!module->enum_types.empty()) {
        out.append("\n");
    }
}

// --- Struct forward declarations ---

void CEmitter::emit_struct_forward_declarations(const IRModule* module, String& out) {
    for (u32 s = 0; s < module->struct_types.size(); s++) {
        Type* struct_type = module->struct_types[s];
        out.append("typedef struct ");
        emit_mangled_name(struct_type->struct_info.name, out);
        out.push_back(' ');
        emit_mangled_name(struct_type->struct_info.name, out);
        out.append(";\n");
    }
    if (!module->struct_types.empty()) {
        out.append("\n");
    }
}

// --- Struct typedefs ---

void CEmitter::emit_struct_typedefs(const IRModule* module, String& out) {
    // Simple dependency sort: emit structs that don't depend on others first.
    // Build adjacency: struct A depends on struct B if A has a value-type field of type B.
    u32 count = module->struct_types.size();
    if (count == 0) return;

    // Map struct type pointer to index
    tsl::robin_map<Type*, u32> type_to_index;
    for (u32 i = 0; i < count; i++) {
        type_to_index[module->struct_types[i]] = i;
    }

    // Build dependency edges: depends_on[i] = list of indices that i depends on
    Vector<Vector<u32>> depends_on(count);
    Vector<u32> in_degree(count, 0);

    for (u32 i = 0; i < count; i++) {
        Type* struct_type = module->struct_types[i];
        for (u32 f = 0; f < struct_type->struct_info.fields.size(); f++) {
            Type* field_type = struct_type->struct_info.fields[f].type;
            if (field_type && field_type->is_struct()) {
                auto it = type_to_index.find(field_type);
                if (it != type_to_index.end()) {
                    depends_on[i].push_back(it->second);
                    in_degree[i]++;
                }
            }
        }
    }

    // Topological sort (Kahn's algorithm)
    Vector<u32> order;
    Vector<u32> queue;
    for (u32 i = 0; i < count; i++) {
        if (in_degree[i] == 0) queue.push_back(i);
    }

    while (!queue.empty()) {
        u32 current = queue.back();
        queue.pop_back();
        order.push_back(current);

        // Find who depends on current
        for (u32 i = 0; i < count; i++) {
            for (u32 d = 0; d < depends_on[i].size(); d++) {
                if (depends_on[i][d] == current) {
                    in_degree[i]--;
                    if (in_degree[i] == 0) queue.push_back(i);
                }
            }
        }
    }

    // If there are remaining (circular deps), just append them
    if (order.size() < count) {
        for (u32 i = 0; i < count; i++) {
            bool found = false;
            for (u32 j = 0; j < order.size(); j++) {
                if (order[j] == i) { found = true; break; }
            }
            if (!found) order.push_back(i);
        }
    }

    // Emit each struct in dependency order
    for (u32 o = 0; o < order.size(); o++) {
        Type* struct_type = module->struct_types[order[o]];
        const StructTypeInfo& info = struct_type->struct_info;

        out.append("struct ");
        emit_mangled_name(info.name, out);
        out.append(" {\n");

        // Emit regular fields
        for (u32 f = 0; f < info.fields.size(); f++) {
            const FieldInfo& field = info.fields[f];

            // Skip the discriminant field if it will be part of a when clause —
            // it's already declared in the fields list, so we emit it normally.

            out.append("    ");
            emit_type(field.type, out);
            out.push_back(' ');
            out.append(field.name.data(), field.name.size());
            out.append(";\n");
        }

        // Emit tagged union anonymous unions (when clauses)
        for (u32 w = 0; w < info.when_clauses.size(); w++) {
            const WhenClauseInfo& clause = info.when_clauses[w];
            out.append("    union {\n");
            for (u32 v = 0; v < clause.variants.size(); v++) {
                const VariantInfo& variant = clause.variants[v];
                if (variant.fields.size() == 0) continue;
                out.append("        struct { ");
                for (u32 vf = 0; vf < variant.fields.size(); vf++) {
                    emit_type(variant.fields[vf].type, out);
                    out.push_back(' ');
                    out.append(variant.fields[vf].name.data(), variant.fields[vf].name.size());
                    out.append("; ");
                }
                out.append("}; /* ");
                out.append(variant.case_name.data(), variant.case_name.size());
                out.append(" */\n");
            }
            out.append("    };\n");
        }

        out.append("};\n\n");
    }
}

// --- Block argument helpers ---

void CEmitter::emit_block_arg_declarations(const IRFunction* func, String& out) {
    for (u32 b = 0; b < func->blocks.size(); b++) {
        const IRBlock* block = func->blocks[b];
        if (block->params.empty()) continue;
        if (b == 0) continue; // entry block params are function params

        for (u32 p = 0; p < block->params.size(); p++) {
            out.append("    ");
            emit_type(block->params[p].type, out);
            char buf[32];
            format_to(buf, sizeof(buf), " block{}_arg{};\n", block->id.id, p);
            out.append(buf);
        }
    }
}

void CEmitter::emit_block_arg_assignments(const JumpTarget& target, String& out) {
    for (u32 i = 0; i < target.args.size(); i++) {
        out.append("    ");
        char buf[64];
        format_to(buf, sizeof(buf), "block{}_arg{} = ", target.block.id, i);
        out.append(buf);
        emit_value(target.args[i].value, out);
        out.append(";\n");
    }
}

// --- Instruction emission ---
// All SSA values are pre-declared at function top. Instructions only assign.

void CEmitter::emit_instruction(const IRInst* inst, String& out) {
    if (inst->op == IROp::BlockArg) return;

    switch (inst->op) {
        // --- Constants ---
        case IROp::ConstBool: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(inst->const_data.bool_val ? " = true;\n" : " = false;\n");
            return;
        }
        case IROp::ConstInt: {
            out.append("    ");
            emit_value(inst->result, out);
            if (inst->type && inst->type->is_enum()) {
                // C++ requires explicit cast for enum types
                out.append(" = (");
                emit_type(inst->type, out);
                out.append(")");
                char buf[32];
                format_to(buf, sizeof(buf), "{};\n", inst->const_data.int_val);
                out.append(buf);
            } else {
                char buf[32];
                if (inst->type && (inst->type->kind == TypeKind::I64 || inst->type->kind == TypeKind::U64)) {
                    format_to(buf, sizeof(buf), " = {}LL;\n", inst->const_data.int_val);
                } else {
                    format_to(buf, sizeof(buf), " = {};\n", inst->const_data.int_val);
                }
                out.append(buf);
            }
            return;
        }
        case IROp::ConstF: {
            out.append("    ");
            emit_value(inst->result, out);
            char buf[48];
            snprintf(buf, sizeof(buf), " = %.9gf;\n", static_cast<double>(inst->const_data.f32_val));
            out.append(buf);
            return;
        }
        case IROp::ConstD: {
            out.append("    ");
            emit_value(inst->result, out);
            char buf[48];
            snprintf(buf, sizeof(buf), " = %.17g;\n", inst->const_data.f64_val);
            out.append(buf);
            return;
        }
        case IROp::ConstNull: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = nullptr;\n");
            return;
        }

        // --- Binary arithmetic (integer) ---
        case IROp::AddI: case IROp::SubI: case IROp::MulI:
        case IROp::DivI: case IROp::ModI: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::AddI: op_str = " + "; break;
                case IROp::SubI: op_str = " - "; break;
                case IROp::MulI: op_str = " * "; break;
                case IROp::DivI: op_str = " / "; break;
                case IROp::ModI: op_str = " % "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Binary arithmetic (f32) ---
        case IROp::AddF: case IROp::SubF: case IROp::MulF: case IROp::DivF: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::AddF: op_str = " + "; break;
                case IROp::SubF: op_str = " - "; break;
                case IROp::MulF: op_str = " * "; break;
                case IROp::DivF: op_str = " / "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Binary arithmetic (f64) ---
        case IROp::AddD: case IROp::SubD: case IROp::MulD: case IROp::DivD: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::AddD: op_str = " + "; break;
                case IROp::SubD: op_str = " - "; break;
                case IROp::MulD: op_str = " * "; break;
                case IROp::DivD: op_str = " / "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Unary arithmetic ---
        case IROp::NegI: case IROp::NegF: case IROp::NegD: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = -");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }

        // --- Comparisons (integer) ---
        case IROp::EqI: case IROp::NeI: case IROp::LtI:
        case IROp::LeI: case IROp::GtI: case IROp::GeI: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::EqI: op_str = " == "; break;
                case IROp::NeI: op_str = " != "; break;
                case IROp::LtI: op_str = " < ";  break;
                case IROp::LeI: op_str = " <= "; break;
                case IROp::GtI: op_str = " > ";  break;
                case IROp::GeI: op_str = " >= "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Comparisons (f32) ---
        case IROp::EqF: case IROp::NeF: case IROp::LtF:
        case IROp::LeF: case IROp::GtF: case IROp::GeF: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::EqF: op_str = " == "; break;
                case IROp::NeF: op_str = " != "; break;
                case IROp::LtF: op_str = " < ";  break;
                case IROp::LeF: op_str = " <= "; break;
                case IROp::GtF: op_str = " > ";  break;
                case IROp::GeF: op_str = " >= "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Comparisons (f64) ---
        case IROp::EqD: case IROp::NeD: case IROp::LtD:
        case IROp::LeD: case IROp::GtD: case IROp::GeD: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::EqD: op_str = " == "; break;
                case IROp::NeD: op_str = " != "; break;
                case IROp::LtD: op_str = " < ";  break;
                case IROp::LeD: op_str = " <= "; break;
                case IROp::GtD: op_str = " > ";  break;
                case IROp::GeD: op_str = " >= "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Logical ---
        case IROp::Not: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = !");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }
        case IROp::And: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(" && ");
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }
        case IROp::Or: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(" || ");
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }

        // --- Bitwise ---
        case IROp::BitAnd: case IROp::BitOr: case IROp::BitXor:
        case IROp::Shl: case IROp::Shr: {
            const char* op_str = nullptr;
            switch (inst->op) {
                case IROp::BitAnd: op_str = " & ";  break;
                case IROp::BitOr:  op_str = " | ";  break;
                case IROp::BitXor: op_str = " ^ ";  break;
                case IROp::Shl:    op_str = " << "; break;
                case IROp::Shr:    op_str = " >> "; break;
                default: break;
            }
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->binary.left, out);
            out.append(op_str);
            emit_value(inst->binary.right, out);
            out.append(";\n");
            return;
        }
        case IROp::BitNot: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ~");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }

        // --- Type conversions ---
        case IROp::I_TO_F64: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = (double)");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }
        case IROp::F64_TO_I: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = (");
            emit_type(inst->type, out);
            out.append(")");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }
        case IROp::I_TO_B: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->unary, out);
            out.append(" != 0;\n");
            return;
        }
        case IROp::B_TO_I: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = (");
            emit_type(inst->type, out);
            out.append(")");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }

        // --- Copy ---
        case IROp::Copy: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            emit_value(inst->unary, out);
            out.append(";\n");
            return;
        }

        // --- Function calls ---
        case IROp::Call: {
            // Look up called function for type info
            const IRFunction* callee = find_function(inst->call.func_name);

            out.append("    ");
            // Skip assignment if callee returns void (e.g., large struct return via hidden ptr)
            bool assign_result = inst->result.is_valid() && inst->type &&
                                 inst->type->kind != TypeKind::Void;
            if (assign_result && callee && callee->returns_large_struct()) {
                assign_result = false;
            }
            if (assign_result) {
                emit_value(inst->result, out);
                out.append(" = ");
            }
            emit_mangled_name(inst->call.func_name, out);
            out.push_back('(');
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                if (i > 0) out.append(", ");

                // Check if we need an explicit cast for struct pointer arguments
                // (e.g., Dog* to Animal* for inheritance)
                bool need_cast = false;
                Type* param_type = nullptr;
                if (callee && i < callee->params.size()) {
                    param_type = callee->params[i].type;
                    Type* arg_type = get_value_type(inst->call.args[i]);

                    // Unwrap reference types to get the underlying struct types
                    Type* arg_inner = arg_type;
                    Type* param_inner = param_type;
                    if (arg_inner && arg_inner->is_reference())
                        arg_inner = arg_inner->ref_info.inner_type;
                    if (param_inner && param_inner->is_reference())
                        param_inner = param_inner->ref_info.inner_type;

                    if (arg_inner && param_inner && arg_inner->is_struct() && param_inner->is_struct() &&
                        arg_inner != param_inner && is_pointer_value(inst->call.args[i])) {
                        need_cast = true;
                        // Use the callee's param inner type for the cast
                        param_type = param_inner;
                    }
                }

                if (need_cast) {
                    out.append("(");
                    emit_type(param_type, out);
                    out.append("*)");
                }
                emit_value(inst->call.args[i], out);
            }
            out.append(");\n");
            return;
        }
        case IROp::CallExternal: {
            out.append("    ");
            if (inst->result.is_valid() && inst->type && inst->type->kind != TypeKind::Void) {
                emit_value(inst->result, out);
                out.append(" = ");
            }
            emit_mangled_name(inst->call_external.module_name, out);
            out.append("__");
            emit_mangled_name(inst->call_external.func_name, out);
            out.push_back('(');
            for (u32 i = 0; i < inst->call_external.args.size(); i++) {
                if (i > 0) out.append(", ");
                emit_value(inst->call_external.args[i], out);
            }
            out.append(");\n");
            return;
        }

        // --- Struct operations ---
        case IROp::StackAlloc: {
            // v5 = &v5_struct;
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = &");
            emit_value(inst->result, out);
            out.append("_struct;\n");
            return;
        }
        case IROp::GetField: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = ");
            if (is_scalar_stack_alloc(inst->field.object)) {
                // Scalar StackAlloc (out/inout): dereference pointer
                out.append("*");
                emit_value(inst->field.object, out);
            } else {
                emit_field_access(inst->field.object, inst->field.field_name, out);
            }
            out.append(";\n");
            return;
        }
        case IROp::SetField: {
            // Synthetic `__zero` field: bulk-zero a slot range. The IR builder
            // emits this from `emit_zero_slots` to clear self's own slots at
            // constructor entry (and to clear union slots before initialising
            // a `when`-clause variant). The Roxy IR slot layout matches the
            // emitted C struct's byte layout (each slot is 4 bytes, fields
            // declared in order with matching widths, anonymous unions sized
            // to their largest variant), so slot_offset/slot_count map
            // directly to byte offsets.
            if (inst->field.field_name == StringView("__zero", 6)) {
                out.append("    memset((char*)");
                if (is_pointer_value(inst->field.object)) {
                    emit_value(inst->field.object, out);
                } else {
                    out.append("&");
                    emit_value(inst->field.object, out);
                    if (is_stack_alloc_value(inst->field.object)) {
                        out.append("_struct");
                    }
                }
                if (inst->field.slot_offset > 0) {
                    char buf[32];
                    format_to(buf, sizeof(buf), " + {}", inst->field.slot_offset * 4);
                    out.append(buf);
                }
                char buf[32];
                format_to(buf, sizeof(buf), ", 0, {});\n", inst->field.slot_count * 4);
                out.append(buf);
                return;
            }
            out.append("    ");
            if (is_scalar_stack_alloc(inst->field.object)) {
                // Scalar StackAlloc (out/inout): store through pointer
                out.append("*");
                emit_value(inst->field.object, out);
            } else {
                emit_field_access(inst->field.object, inst->field.field_name, out);
            }
            out.append(" = ");
            emit_value(inst->store_value, out);
            out.append(";\n");
            return;
        }
        case IROp::GetFieldAddr: {
            // v7 = &v5->inner; or v7 = &v5.inner;
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = &");
            emit_field_access(inst->field.object, inst->field.field_name, out);
            out.append(";\n");
            return;
        }
        case IROp::StructCopy: {
            // memcpy(dest, src, slot_count * 4);
            out.append("    memcpy(");
            emit_value(inst->struct_copy.dest_ptr, out);
            out.append(", ");
            emit_value(inst->struct_copy.source_ptr, out);
            char buf[32];
            format_to(buf, sizeof(buf), ", {});\n", inst->struct_copy.slot_count * 4);
            out.append(buf);
            return;
        }

        // --- Pointer operations ---
        case IROp::LoadPtr: {
            // v1 = *v0;
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = *");
            emit_value(inst->load_ptr.ptr, out);
            out.append(";\n");
            return;
        }
        case IROp::StorePtr: {
            // *v0 = v1;
            out.append("    *");
            emit_value(inst->store_ptr.ptr, out);
            out.append(" = ");
            emit_value(inst->store_ptr.value, out);
            out.append(";\n");
            return;
        }
        case IROp::VarAddr: {
            // v1 = &v0;  or  v1 = &v0_struct;
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = &");

            // Look up the variable name to find the ValueId
            StringView var_name = inst->var_addr.name;
            u32 name_hash = 0;
            for (u32 c = 0; c < var_name.size(); c++) {
                name_hash = name_hash * 31 + var_name.data()[c];
            }

            auto it = m_var_name_to_value.find(name_hash);
            if (it != m_var_name_to_value.end()) {
                ValueId target_value = it->second;
                if (is_stack_alloc_value(target_value)) {
                    emit_value(target_value, out);
                    out.append("_struct;\n");
                } else {
                    emit_value(target_value, out);
                    out.append(";\n");
                }
            } else {
                // Fallback: emit a comment and abort
                out.append("0; /* VarAddr: unknown variable '");
                out.append(var_name.data(), var_name.size());
                out.append("' */\n");
            }
            return;
        }

        // --- Cast ---
        case IROp::Cast: {
            // v1 = (TargetType)v0;
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = (");
            emit_type(inst->type, out);
            out.append(")");
            emit_value(inst->cast.source, out);
            out.append(";\n");
            return;
        }

        // --- Nullify ---
        case IROp::Nullify: {
            // Zero out a value for move semantics cleanup
            Type* val_type = get_value_type(inst->unary);
            if (val_type && val_type->is_struct() && is_stack_alloc_value(inst->unary)) {
                // memset for struct backing storage
                out.append("    memset(&");
                emit_value(inst->unary, out);
                out.append("_struct, 0, sizeof(");
                emit_value(inst->unary, out);
                out.append("_struct));\n");
            } else {
                // For primitives/pointers
                out.append("    ");
                emit_value(inst->unary, out);
                out.append(" = 0;\n");
            }
            return;
        }

        // --- Phase 3: Runtime library ops ---

        case IROp::ConstString: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = roxy_string_from_literal(");
            emit_escaped_string(inst->const_data.string_val, out);
            char buf[16];
            format_to(buf, sizeof(buf), ", {});\n", inst->const_data.string_val.size());
            out.append(buf);
            return;
        }

        case IROp::CallNative: {
            emit_native_call(inst, out);
            return;
        }

        case IROp::IndexGet: {
            // container[index] — runtime returns void* into backing storage.
            // For struct value type, the result local is a struct pointer (via
            // collect_value_types). For primitives, dereference at the call
            // site so C's typed deref handles widening / sign-extension.
            Type* val_type = inst->type;
            bool is_struct_val = val_type && val_type->is_struct();
            bool is_map = inst->index_data.kind == ContainerKind::Map;
            const char* fn = is_map ? "roxy_map_get" : "roxy_list_get";
            // For maps, the key is now a `const void*` to bytes. Primitive
            // keys are bit-copied into a 2-slot uint64_t temp so the runtime
            // sees the full 8 bytes per key (matches the runtime's
            // 2-slot inline key shape). Struct keys pass their pointer
            // directly.
            Type* key_type = is_map ? get_value_type(inst->index_data.index) : nullptr;
            bool key_is_struct = key_type && key_type->is_struct();
            bool needs_key_temp = is_map && !key_is_struct;
            if (needs_key_temp) {
                out.append("    { uint64_t _ktmp = 0; { ");
                emit_type(key_type, out);
                out.append(" _kraw = ");
                emit_value(inst->index_data.index, out);
                out.append("; memcpy(&_ktmp, &_kraw, sizeof(_kraw)); } ");
            } else {
                out.append("    ");
            }
            emit_value(inst->result, out);
            out.append(" = ");
            if (is_struct_val) {
                out.append("(");
                emit_type(val_type, out);
                out.append("*)");
            } else {
                out.append("*(");
                emit_type(val_type, out);
                out.append("*)");
            }
            out.append(fn);
            out.append("((void*)");
            emit_value(inst->index_data.container, out);
            out.append(", ");
            if (is_map) {
                if (key_is_struct) {
                    emit_value(inst->index_data.index, out);
                } else {
                    out.append("&_ktmp");
                }
            } else {
                emit_value(inst->index_data.index, out);
            }
            out.append(");");
            if (needs_key_temp) out.append(" }");
            out.append("\n");
            return;
        }

        case IROp::IndexSet: {
            // container[index] = value — runtime takes `const void*` pointers
            // to key/value bytes. Struct args pass `vN` directly; primitive
            // args get brace-scoped stack temps. Primitive keys use a u64
            // temp with bit-copied key bytes (matches 2-slot inline key);
            // primitive values use a typed temp matching value_slot_count.
            Type* val_type = get_value_type(inst->index_data.value);
            bool is_struct_val = val_type && val_type->is_struct();
            bool is_map = inst->index_data.kind == ContainerKind::Map;
            const char* fn = is_map ? "roxy_map_insert" : "roxy_list_set";
            Type* key_type = is_map ? get_value_type(inst->index_data.index) : nullptr;
            bool key_is_struct = key_type && key_type->is_struct();
            bool needs_key_temp = is_map && !key_is_struct;
            bool needs_brace = needs_key_temp || !is_struct_val;
            if (needs_brace) {
                out.append("    { ");
                if (needs_key_temp) {
                    out.append("uint64_t _ktmp = 0; { ");
                    emit_type(key_type, out);
                    out.append(" _kraw = ");
                    emit_value(inst->index_data.index, out);
                    out.append("; memcpy(&_ktmp, &_kraw, sizeof(_kraw)); } ");
                }
                if (!is_struct_val) {
                    emit_type(val_type, out);
                    out.append(" _vtmp = ");
                    emit_value(inst->index_data.value, out);
                    out.append("; ");
                }
            } else {
                out.append("    ");
            }
            out.append(fn);
            out.append("((void*)");
            emit_value(inst->index_data.container, out);
            out.append(", ");
            if (is_map) {
                if (key_is_struct) {
                    emit_value(inst->index_data.index, out);
                } else {
                    out.append("&_ktmp");
                }
            } else {
                // List: index is int32_t, no pointer wrapping
                emit_value(inst->index_data.index, out);
            }
            out.append(", ");
            if (is_struct_val) {
                emit_value(inst->index_data.value, out);
            } else {
                out.append("&_vtmp");
            }
            out.append(");");
            if (needs_brace) out.append(" }");
            out.append("\n");
            return;
        }

        case IROp::New: {
            // Allocate a new heap object: (StructType*)roxy_alloc(sizeof(StructType), TYPEID_StructType)
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = (");
            emit_type(inst->type, out);
            out.append(")roxy_alloc(sizeof(");
            // The result type is a pointer (Uniq/Ref), so get the inner struct type
            Type* inner_type = inst->type;
            if (inner_type && inner_type->is_reference()) {
                inner_type = inner_type->ref_info.inner_type;
            }
            if (inner_type) {
                emit_type(inner_type, out);
            } else {
                emit_type(inst->type, out);
            }
            out.append("), TYPEID_");
            emit_mangled_name(inst->new_data.type_name, out);
            out.append(");\n");
            // Zero-initialize the struct data
            out.append("    memset(");
            emit_value(inst->result, out);
            out.append(", 0, sizeof(");
            if (inner_type) {
                emit_type(inner_type, out);
            } else {
                emit_type(inst->type, out);
            }
            out.append("));\n");
            return;
        }

        case IROp::Delete: {
            if (!inst->type || inst->type->kind == TypeKind::Void) {
                // Raw free (explicit delete after manual destructor call)
                out.append("    roxy_free(");
                emit_value(inst->unary, out);
                out.append(");\n");
            } else {
                // TODO: emit inline typed delete for C backend
                // For now, emit raw free as a fallback
                out.append("    roxy_free(");
                emit_value(inst->unary, out);
                out.append("); /* TODO: typed delete */\n");
            }
            return;
        }

        case IROp::RefInc: {
            out.append("    roxy_ref_inc(");
            emit_value(inst->unary, out);
            out.append(");\n");
            return;
        }

        case IROp::RefDec: {
            out.append("    roxy_ref_dec(");
            emit_value(inst->unary, out);
            out.append(");\n");
            return;
        }

        case IROp::WeakCreate: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = roxy_weak_create(");
            emit_value(inst->unary, out);
            out.append(");\n");
            return;
        }

        case IROp::WeakCheck: {
            // WeakCheck takes a roxy_weak struct and checks validity
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = roxy_weak_valid(");
            emit_value(inst->unary, out);
            out.append(".ptr, ");
            emit_value(inst->unary, out);
            out.append(".generation);\n");
            return;
        }

        // --- Still unsupported (Phase 4+) ---
        case IROp::Throw:
        case IROp::Yield:
        case IROp::Closure:
        case IROp::CallIndirect:
        case IROp::AssertHeap: {
            out.append("    /* TODO: unsupported op: ");
            out.append(ir_op_to_string(inst->op));
            out.append(" */\n");
            out.append("    abort();\n");
            return;
        }

        case IROp::BlockArg:
            return;
    }
}

// --- Terminator emission ---

void CEmitter::emit_terminator(const IRBlock* block, const IRFunction* func, String& out) {
    const Terminator& term = block->terminator;

    switch (term.kind) {
        case TerminatorKind::Goto: {
            emit_block_arg_assignments(term.goto_target, out);
            char buf[32];
            format_to(buf, sizeof(buf), "    goto block{};\n", term.goto_target.block.id);
            out.append(buf);
            return;
        }
        case TerminatorKind::Branch: {
            out.append("    if (");
            emit_value(term.branch.condition, out);
            out.append(") {\n");
            // Then branch
            for (u32 i = 0; i < term.branch.then_target.args.size(); i++) {
                char buf[64];
                format_to(buf, sizeof(buf), "        block{}_arg{} = ", term.branch.then_target.block.id, i);
                out.append(buf);
                emit_value(term.branch.then_target.args[i].value, out);
                out.append(";\n");
            }
            {
                char buf[32];
                format_to(buf, sizeof(buf), "        goto block{};\n", term.branch.then_target.block.id);
                out.append(buf);
            }
            out.append("    } else {\n");
            // Else branch
            for (u32 i = 0; i < term.branch.else_target.args.size(); i++) {
                char buf[64];
                format_to(buf, sizeof(buf), "        block{}_arg{} = ", term.branch.else_target.block.id, i);
                out.append(buf);
                emit_value(term.branch.else_target.args[i].value, out);
                out.append(";\n");
            }
            {
                char buf[32];
                format_to(buf, sizeof(buf), "        goto block{};\n", term.branch.else_target.block.id);
                out.append(buf);
            }
            out.append("    }\n");
            return;
        }
        case TerminatorKind::Return: {
            if (term.return_value.is_valid()) {
                // If returning a struct from a StackAlloc pointer, dereference it
                Type* ret_type = func->return_type;
                if (ret_type && ret_type->is_struct() && !func->returns_large_struct() &&
                    is_stack_alloc_value(term.return_value)) {
                    out.append("    return *");
                    emit_value(term.return_value, out);
                    out.append(";\n");
                } else {
                    out.append("    return ");
                    emit_value(term.return_value, out);
                    out.append(";\n");
                }
            } else {
                out.append("    return;\n");
            }
            return;
        }
        case TerminatorKind::Unreachable: {
            out.append("    __builtin_unreachable();\n");
            return;
        }
        case TerminatorKind::None: {
            out.append("    /* unterminated block */\n");
            return;
        }
    }
}

// --- Block emission ---

void CEmitter::emit_block(const IRBlock* block, const IRFunction* func, String& out) {
    // Emit label (semicolon after label required for empty blocks or when next is a declaration)
    char label_buf[32];
    format_to(label_buf, sizeof(label_buf), "block{}:;\n", block->id.id);
    out.append(label_buf);

    // Read block parameters from arg-passing variables
    for (u32 p = 0; p < block->params.size(); p++) {
        out.append("    ");
        emit_value(block->params[p].value, out);
        char buf[32];
        format_to(buf, sizeof(buf), " = block{}_arg{};\n", block->id.id, p);
        out.append(buf);
    }

    // Emit instructions
    for (u32 i = 0; i < block->instructions.size(); i++) {
        emit_instruction(block->instructions[i], out);
    }

    // Emit terminator
    emit_terminator(block, func, out);
}

// --- Function prototype ---

void CEmitter::emit_function_prototype(const IRFunction* func, String& out) {
    // When emit_main_entry is true and this is the "main" function,
    // use `int` return type for C/C++ standard compliance
    bool is_main = m_config.emit_main_entry && func->name == StringView("main");

    if (is_main) {
        out.append("int");
    } else if (func->returns_large_struct()) {
        // Large struct returns use hidden output pointer — return void
        out.append("void");
    } else {
        emit_type(func->return_type, out);
    }
    out.push_back(' ');
    emit_mangled_name(func->name, out);
    out.push_back('(');

    if (func->params.empty()) {
        out.append("void");
    } else {
        for (u32 i = 0; i < func->params.size(); i++) {
            if (i > 0) out.append(", ");
            Type* param_type = func->params[i].type;
            emit_type(param_type, out);
            // Struct params are always passed by pointer in the IR
            // (IR uses get_field/set_field with pointer semantics on all struct values)
            if (param_type && param_type->is_struct()) {
                out.push_back('*');
            }
            // Add pointer for out/inout params that aren't already reference/struct types
            else if (i < func->param_is_ptr.size() && func->param_is_ptr[i]) {
                if (!param_type || (!param_type->is_reference())) {
                    out.push_back('*');
                }
            }
            out.push_back(' ');
            emit_value(func->params[i].value, out);
        }
    }

    out.push_back(')');
}

// --- Function emission ---

void CEmitter::emit_function(const IRFunction* func, String& out) {
    collect_value_types(func);

    emit_function_prototype(func, out);
    out.append(" {\n");

    // Declare ALL SSA value locals at the top of the function body.
    // This avoids C/C++ issues with goto jumping over variable declarations.

    // First, collect which ValueIds are function params (already in signature)
    tsl::robin_set<u32> is_func_param;
    for (u32 i = 0; i < func->params.size(); i++) {
        is_func_param.insert(func->params[i].value.id);
    }

    // Declare instruction result values
    for (u32 b = 0; b < func->blocks.size(); b++) {
        const IRBlock* block = func->blocks[b];
        for (u32 i = 0; i < block->instructions.size(); i++) {
            const IRInst* inst = block->instructions[i];
            if (inst->op == IROp::BlockArg) continue;
            if (!inst->result.is_valid()) continue;
            if (!inst->type || inst->type->kind == TypeKind::Void) continue;

            if (inst->op == IROp::StackAlloc) {
                // StackAlloc needs TWO declarations: backing storage + pointer
                Type* alloc_type = inst->type;
                if (alloc_type && alloc_type->is_struct()) {
                    // Backing storage: StructType v5_struct; memset(&v5_struct, 0, sizeof(v5_struct));
                    out.append("    ");
                    emit_type(alloc_type, out);
                    out.push_back(' ');
                    emit_value(inst->result, out);
                    out.append("_struct; memset(&");
                    emit_value(inst->result, out);
                    out.append("_struct, 0, sizeof(");
                    emit_value(inst->result, out);
                    out.append("_struct));\n");
                    // Pointer: StructType* v5;
                    out.append("    ");
                    emit_type(alloc_type, out);
                    out.append("* ");
                    emit_value(inst->result, out);
                    out.append(";\n");
                } else if (alloc_type && !alloc_type->is_void()) {
                    // Scalar stack alloc (for out/inout parameters on scalars)
                    out.append("    ");
                    emit_type(alloc_type, out);
                    out.push_back(' ');
                    emit_value(inst->result, out);
                    out.append("_struct; memset(&");
                    emit_value(inst->result, out);
                    out.append("_struct, 0, sizeof(");
                    emit_value(inst->result, out);
                    out.append("_struct));\n");
                    out.append("    ");
                    emit_type(alloc_type, out);
                    out.append("* ");
                    emit_value(inst->result, out);
                    out.append(";\n");
                } else {
                    // Fallback: use raw byte array
                    out.append("    uint8_t ");
                    emit_value(inst->result, out);
                    char buf[32];
                    format_to(buf, sizeof(buf), "_struct[{}] = {{0}};\n", inst->stack_alloc.slot_count * 4);
                    out.append(buf);
                    out.append("    void* ");
                    emit_value(inst->result, out);
                    out.append(";\n");
                }
                continue;
            }

            if (inst->op == IROp::GetFieldAddr) {
                // GetFieldAddr returns a pointer to the field
                out.append("    ");
                emit_type(inst->type, out);
                out.append("* ");
                emit_value(inst->result, out);
                out.append(";\n");
                continue;
            }

            out.append("    ");
            emit_type(inst->type, out);
            // Struct-typed results that are pointer-tracked (IndexGet of a
            // struct, getter natives) are stored as `StructType* vN;`.
            if (is_pointer_value(inst->result) && inst->type && inst->type->is_struct()) {
                out.append("* ");
            } else {
                out.push_back(' ');
            }
            emit_value(inst->result, out);
            out.append(";\n");
        }
    }

    // Declare block parameter values (non-entry blocks)
    for (u32 b = 1; b < func->blocks.size(); b++) {
        const IRBlock* block = func->blocks[b];
        for (u32 p = 0; p < block->params.size(); p++) {
            out.append("    ");
            emit_type(block->params[p].type, out);
            out.push_back(' ');
            emit_value(block->params[p].value, out);
            out.append(";\n");
        }
    }

    // Declare block argument passing variables
    emit_block_arg_declarations(func, out);

    // Emit blocks
    for (u32 b = 0; b < func->blocks.size(); b++) {
        emit_block(func->blocks[b], func, out);
    }

    out.append("}\n");
}

// --- Top-level emission ---

void CEmitter::emit_header(const IRModule* module, String& output) {
    output.append("#pragma once\n\n");
    output.append("#include <stdint.h>\n");
    output.append("#include <stdbool.h>\n\n");
}

// --- Phase 3: Runtime library support ---

void CEmitter::emit_runtime_include(String& out) {
    out.append("#include \"roxy_rt.h\"\n");
}

void CEmitter::emit_type_id_defines(const IRModule* module, String& out) {
    // User-defined struct type IDs start at 100
    for (u32 i = 0; i < module->struct_types.size(); i++) {
        out.append("#define TYPEID_");
        emit_mangled_name(module->struct_types[i]->struct_info.name, out);
        char buf[16];
        format_to(buf, sizeof(buf), " {}\n", 100 + i);
        out.append(buf);
    }
    if (!module->struct_types.empty()) {
        out.append("\n");
    }
}

void CEmitter::emit_escaped_string(StringView str, String& out) {
    out.push_back('"');
    const char* data = str.data();
    u32 len = str.size();
    for (u32 i = 0; i < len; i++) {
        char c = data[i];
        switch (c) {
            case '\\': out.append("\\\\"); break;
            case '"':  out.append("\\\""); break;
            case '\n': out.append("\\n"); break;
            case '\r': out.append("\\r"); break;
            case '\t': out.append("\\t"); break;
            case '\0': out.append("\\0"); break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\x%02x", static_cast<unsigned char>(c));
                    out.append(buf);
                } else {
                    out.push_back(c);
                }
                break;
        }
    }
    out.push_back('"');
}

void CEmitter::emit_native_call(const IRInst* inst, String& out) {
    StringView name = inst->call.func_name;

    // Static name mapping table
    struct NativeMapping {
        const char* roxy_name;
        const char* c_name;
    };

    static const NativeMapping mappings[] = {
        // Print
        {"print", "roxy_print"},
        // String functions
        {"str_concat", "roxy_string_concat"},
        {"str_eq", "roxy_string_eq"},
        {"str_ne", "roxy_string_ne"},
        {"str_len", "roxy_string_len"},
        // to_string
        {"bool$$to_string", "roxy_bool_to_string"},
        {"i32$$to_string", "roxy_i32_to_string"},
        {"i64$$to_string", "roxy_i64_to_string"},
        {"f32$$to_string", "roxy_f32_to_string"},
        {"f64$$to_string", "roxy_f64_to_string"},
        {"string$$to_string", "roxy_string_to_string"},
        // Hash
        {"bool$$hash", "roxy_bool_hash"},
        {"i8$$hash", "roxy_i8_hash"},
        {"i16$$hash", "roxy_i16_hash"},
        {"i32$$hash", "roxy_i32_hash"},
        {"i64$$hash", "roxy_i64_hash"},
        {"u8$$hash", "roxy_u8_hash"},
        {"u16$$hash", "roxy_u16_hash"},
        {"u32$$hash", "roxy_u32_hash"},
        {"u64$$hash", "roxy_u64_hash"},
        {"f32$$hash", "roxy_f32_hash"},
        {"f64$$hash", "roxy_f64_hash"},
        {"string$$hash", "roxy_string_hash"},
        // List
        {"list_alloc", "roxy_list_alloc"},
        {"list_copy", "roxy_list_copy"},
        {"List$$new", "roxy_list_init"},
        {"List$$delete", "roxy_list_delete"},
        {"List$$len", "roxy_list_len"},
        {"List$$cap", "roxy_list_cap"},
        {"List$$push", "roxy_list_push"},
        {"List$$pop", "roxy_list_pop"},
        {"List$$index", "roxy_list_get"},
        {"List$$index_mut", "roxy_list_set"},
        // Map
        {"map_alloc", "roxy_map_alloc"},
        {"map_copy", "roxy_map_copy"},
        {"Map$$new", "roxy_map_init"},
        {"Map$$delete", "roxy_map_delete"},
        {"Map$$len", "roxy_map_len"},
        {"Map$$contains", "roxy_map_contains"},
        {"Map$$get", "roxy_map_get"},
        {"Map$$insert", "roxy_map_insert"},
        {"Map$$remove", "roxy_map_remove"},
        {"Map$$clear", "roxy_map_clear"},
        {"Map$$keys", "roxy_map_keys"},
        {"Map$$values", "roxy_map_values"},
        {"Map$$index", "roxy_map_index"},
        {"Map$$index_mut", "roxy_map_index_mut"},
        // Internal map iteration
        {"__map_iter_capacity", "roxy_map_iter_capacity"},
        {"__map_iter_next_occupied", "roxy_map_iter_next_occupied"},
        {"__map_iter_key_at", "roxy_map_iter_key_at"},
        {"__map_iter_value_at", "roxy_map_iter_value_at"},
    };

    const char* c_func_name = nullptr;

    // Check exact matches first
    for (const auto& m : mappings) {
        if (name == StringView(m.roxy_name)) {
            c_func_name = m.c_name;
            break;
        }
    }

    // If no exact match found, check for pattern-based matches
    // (e.g., monomorphized generic names like "List$i32$$$push")
    if (!c_func_name) {
        // Check for List methods with monomorphized names (List$<type>$$method)
        if (name.size() > 4 && name.data()[0] == 'L' && name.data()[1] == 'i' &&
            name.data()[2] == 's' && name.data()[3] == 't') {
            // Find the last $$ in the name to get the method part
            const char* data = name.data();
            u32 len = name.size();
            for (u32 i = len - 1; i >= 2; i--) {
                if (data[i - 1] == '$' && data[i] == '$' && i + 1 < len) {
                    StringView method_part(data + i + 1, len - i - 1);
                    if (method_part == StringView("new")) { c_func_name = "roxy_list_init"; break; }
                    if (method_part == StringView("delete")) { c_func_name = "roxy_list_delete"; break; }
                    if (method_part == StringView("len")) { c_func_name = "roxy_list_len"; break; }
                    if (method_part == StringView("cap")) { c_func_name = "roxy_list_cap"; break; }
                    if (method_part == StringView("push")) { c_func_name = "roxy_list_push"; break; }
                    if (method_part == StringView("pop")) { c_func_name = "roxy_list_pop"; break; }
                    if (method_part == StringView("index")) { c_func_name = "roxy_list_get"; break; }
                    if (method_part == StringView("index_mut")) { c_func_name = "roxy_list_set"; break; }
                    break;
                }
            }
        }

        // Check for Map methods with monomorphized names
        if (!c_func_name && name.size() > 3 && name.data()[0] == 'M' && name.data()[1] == 'a' &&
            name.data()[2] == 'p') {
            const char* data = name.data();
            u32 len = name.size();
            for (u32 i = len - 1; i >= 2; i--) {
                if (data[i - 1] == '$' && data[i] == '$' && i + 1 < len) {
                    StringView method_part(data + i + 1, len - i - 1);
                    if (method_part == StringView("new")) { c_func_name = "roxy_map_init"; break; }
                    if (method_part == StringView("delete")) { c_func_name = "roxy_map_delete"; break; }
                    if (method_part == StringView("len")) { c_func_name = "roxy_map_len"; break; }
                    if (method_part == StringView("contains")) { c_func_name = "roxy_map_contains"; break; }
                    if (method_part == StringView("get")) { c_func_name = "roxy_map_get"; break; }
                    if (method_part == StringView("insert")) { c_func_name = "roxy_map_insert"; break; }
                    if (method_part == StringView("remove")) { c_func_name = "roxy_map_remove"; break; }
                    if (method_part == StringView("clear")) { c_func_name = "roxy_map_clear"; break; }
                    if (method_part == StringView("keys")) { c_func_name = "roxy_map_keys"; break; }
                    if (method_part == StringView("values")) { c_func_name = "roxy_map_values"; break; }
                    if (method_part == StringView("index")) { c_func_name = "roxy_map_index"; break; }
                    if (method_part == StringView("index_mut")) { c_func_name = "roxy_map_index_mut"; break; }
                    break;
                }
            }
        }

        // Check for monomorphized list_alloc / list_copy / map_alloc / map_copy
        if (!c_func_name) {
            // list_alloc$<type> or map_alloc$<type>
            if (name.size() > 10 && StringView(name.data(), 10) == StringView("list_alloc")) {
                c_func_name = "roxy_list_alloc";
            } else if (name.size() > 9 && StringView(name.data(), 9) == StringView("list_copy")) {
                c_func_name = "roxy_list_copy";
            } else if (name.size() > 9 && StringView(name.data(), 9) == StringView("map_alloc")) {
                c_func_name = "roxy_map_alloc";
            } else if (name.size() > 8 && StringView(name.data(), 8) == StringView("map_copy")) {
                c_func_name = "roxy_map_copy";
            }
        }
    }

    if (!c_func_name) {
        // Fallback: emit as mangled name with a warning comment
        out.append("    /* WARNING: unmapped native '");
        out.append(name.data(), name.size());
        out.append("' */ ");
        if (inst->result.is_valid() && inst->type && inst->type->kind != TypeKind::Void) {
            emit_value(inst->result, out);
            out.append(" = ");
        }
        emit_mangled_name(name, out);
        out.push_back('(');
        for (u32 i = 0; i < inst->call.args.size(); i++) {
            if (i > 0) out.append(", ");
            emit_value(inst->call.args[i], out);
        }
        out.append(");\n");
        return;
    }

    // Determine if this is a list/map operation that needs type-erasure casts
    auto name_eq = [](const char* a, const char* b) { return strcmp(a, b) == 0; };
    bool is_list_init = name_eq(c_func_name, "roxy_list_init");
    bool is_list_push = name_eq(c_func_name, "roxy_list_push");
    bool is_list_set = name_eq(c_func_name, "roxy_list_set");
    bool is_list_pop = name_eq(c_func_name, "roxy_list_pop");
    bool is_list_get = name_eq(c_func_name, "roxy_list_get");
    bool is_map_init = name_eq(c_func_name, "roxy_map_init");
    bool is_map_insert = name_eq(c_func_name, "roxy_map_insert");
    bool is_map_get = name_eq(c_func_name, "roxy_map_get");
    bool is_map_contains = name_eq(c_func_name, "roxy_map_contains");
    bool is_map_index = name_eq(c_func_name, "roxy_map_index");
    bool is_map_index_mut = name_eq(c_func_name, "roxy_map_index_mut");
    bool is_map_remove = name_eq(c_func_name, "roxy_map_remove");
    bool is_map_iter_key_at = name_eq(c_func_name, "roxy_map_iter_key_at");
    bool is_map_iter_value_at = name_eq(c_func_name, "roxy_map_iter_value_at");
    bool is_map_alloc = name_eq(c_func_name, "roxy_map_alloc");

    // Pointer-passing natives: which arg slots receive `const void*` to
    // bytes. Map keys live at index 1; values at index 2 (insert/index_mut).
    // List values at index 1 (push) or 2 (set).
    int key_arg_idx = -1;     // map key arg (always idx 1)
    int value_arg_idx = -1;   // value arg
    if (is_list_push)                            value_arg_idx = 1;
    else if (is_list_set)                        value_arg_idx = 2;
    else if (is_map_insert || is_map_index_mut) { key_arg_idx = 1; value_arg_idx = 2; }
    else if (is_map_contains || is_map_get || is_map_index || is_map_remove) { key_arg_idx = 1; }

    Type* key_arg_type = nullptr;
    bool key_arg_is_struct = false;
    if (key_arg_idx >= 0 && static_cast<u32>(key_arg_idx) < inst->call.args.size()) {
        key_arg_type = get_value_type(inst->call.args[key_arg_idx]);
        if (key_arg_type && key_arg_type->is_struct()) key_arg_is_struct = true;
    }
    Type* value_arg_type = nullptr;
    bool value_arg_is_struct = false;
    if (value_arg_idx >= 0 && static_cast<u32>(value_arg_idx) < inst->call.args.size()) {
        value_arg_type = get_value_type(inst->call.args[value_arg_idx]);
        if (value_arg_type && value_arg_type->is_struct()) value_arg_is_struct = true;
    }

    // Pointer-returning natives: result is `void*` from runtime.
    // For struct return type → cast to (T*); for primitive → deref via *(T*).
    // map_iter_key_at / map_iter_value_at return uint64_t directly (need a
    // C-style cast to inst->type so e.g. an int32_t result narrows correctly).
    bool returns_value_ptr = is_list_pop || is_list_get || is_map_get || is_map_index;
    bool returns_value_u64 = is_map_iter_key_at || is_map_iter_value_at;
    bool result_is_struct = inst->type && inst->type->is_struct();

    // Brace-scoped stack temp for primitive key/value args we need to pass by
    // address. Skipped when the arg is already a struct pointer.
    bool needs_key_temp = key_arg_idx >= 0 && !key_arg_is_struct && key_arg_type;
    bool needs_value_temp = value_arg_idx >= 0 && !value_arg_is_struct && value_arg_type;
    bool needs_brace = needs_key_temp || needs_value_temp;

    if (needs_brace) {
        out.append("    { ");
        if (needs_key_temp) {
            // 2-slot uint64_t temp with bit-copied key (matches runtime's
            // 2-slot inline key shape; works for any primitive ≤ 8 bytes).
            out.append("uint64_t _ktmp = 0; { ");
            emit_type(key_arg_type, out);
            out.append(" _kraw = ");
            emit_value(inst->call.args[key_arg_idx], out);
            out.append("; memcpy(&_ktmp, &_kraw, sizeof(_kraw)); } ");
        }
        if (needs_value_temp) {
            emit_type(value_arg_type, out);
            out.append(" _vtmp = ");
            emit_value(inst->call.args[value_arg_idx], out);
            out.append("; ");
        }
    } else {
        out.append("    ");
    }

    bool has_result = inst->result.is_valid() && inst->type && inst->type->kind != TypeKind::Void;
    if (has_result) {
        emit_value(inst->result, out);
        out.append(" = ");
        if (returns_value_ptr) {
            if (result_is_struct) {
                out.append("(");
                emit_type(inst->type, out);
                out.append("*)");
            } else {
                out.append("*(");
                emit_type(inst->type, out);
                out.append("*)");
            }
        } else if (returns_value_u64) {
            out.append("(");
            emit_type(inst->type, out);
            out.append(")");
        }
    }
    out.append(c_func_name);
    out.push_back('(');
    for (u32 i = 0; i < inst->call.args.size(); i++) {
        if (i > 0) out.append(", ");

        // Key arg: struct keys pass pointer directly; primitive keys → &_ktmp.
        if (static_cast<int>(i) == key_arg_idx) {
            if (key_arg_is_struct) {
                emit_value(inst->call.args[i], out);
            } else {
                out.append("&_ktmp");
            }
            continue;
        }
        // Value arg: struct values pass pointer directly; primitive → &_vtmp.
        if (static_cast<int>(i) == value_arg_idx) {
            if (value_arg_is_struct) {
                emit_value(inst->call.args[i], out);
            } else {
                out.append("&_vtmp");
            }
            continue;
        }

        // roxy_map_alloc args 4 (hash_fn_index) and 5 (eq_fn_index): the IR
        // builder passes these as ConstInt values (function indices into the
        // IR module's function table, or -1 for "no custom impl"). The C
        // runtime's signature wants function pointers (or nullptr). Resolve
        // the int constant to the user's mangled C function name and emit
        // its address (or nullptr for -1). The user's `K__hash(K*)` /
        // `K__eq(K*, K*)` C signatures don't match `roxy_map_hash_fn` /
        // `roxy_map_eq_fn` exactly (`K*` vs `const void*`), so we cast.
        if (is_map_alloc && (i == 4 || i == 5)) {
            auto const_it = m_const_int_values.find(inst->call.args[i].id);
            i64 fn_idx = (const_it != m_const_int_values.end()) ? const_it->second : -1;
            if (fn_idx < 0 || fn_idx >= static_cast<i64>(m_module->functions.size())) {
                out.append("nullptr");
            } else {
                StringView fn_name = m_module->functions[static_cast<u32>(fn_idx)]->name;
                const char* cast = (i == 4) ? "(roxy_map_hash_fn)" : "(roxy_map_eq_fn)";
                out.append(cast);
                out.append("&");
                emit_mangled_name(fn_name, out);
            }
            continue;
        }

        emit_value(inst->call.args[i], out);
    }

    // List$$new(self) -> roxy_list_init(self, 0) - add default capacity
    if (is_list_init && inst->call.args.size() == 1) {
        out.append(", 0");
    }
    // Map$$new(self, key_kind) -> roxy_map_init(self, key_kind, 0) - add default capacity
    if (is_map_init && inst->call.args.size() == 2) {
        out.append(", 0");
    }

    out.append(");");
    if (needs_brace) out.append(" }");
    out.append("\n");
}

void CEmitter::emit_source(const IRModule* module, String& output) {
    m_module = module;

    // Includes
    output.append("#include <stdint.h>\n");
    output.append("#include <stdbool.h>\n");
    output.append("#include <stdio.h>\n");
    output.append("#include <stdlib.h>\n");
    output.append("#include <string.h>\n");

    // Runtime library include
    emit_runtime_include(output);
    output.append("\n");

    for (u32 i = 0; i < m_config.native_include_paths.size(); i++) {
        output.append("#include \"");
        output.append(StringView(m_config.native_include_paths[i]));
        output.append("\"\n");
    }
    if (!m_config.native_include_paths.empty()) {
        output.append("\n");
    }

    // Type ID defines for user-defined structs
    emit_type_id_defines(module, output);

    // Enum typedefs
    emit_enum_typedefs(module, output);

    // Struct forward declarations
    emit_struct_forward_declarations(module, output);

    // Struct typedefs (in dependency order)
    emit_struct_typedefs(module, output);

    // Forward declare all functions
    for (u32 i = 0; i < module->functions.size(); i++) {
        emit_function_prototype(module->functions[i], output);
        output.append(";\n");
    }
    output.append("\n");

    // Emit all function bodies
    for (u32 i = 0; i < module->functions.size(); i++) {
        emit_function(module->functions[i], output);
        output.append("\n");
    }
}

} // namespace rx

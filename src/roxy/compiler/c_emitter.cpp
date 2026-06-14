#include "roxy/compiler/c_emitter.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/core/format.hpp"
#include "roxy/core/static_string.hpp"
#include "roxy/vm/binding/registry.hpp"

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
        case TypeKind::Coroutine:
            // Coro<T> is a pointer to its synthesized state struct (__coro_<func>).
            // Matches `uniq __coro_<func>` (New result) and `ref __coro_<func>`
            // (resume/done/$$delete self param), which both emit as the same ptr.
            emit_type(type->coro_info.generated_struct_type, out);
            out.append("*");
            break;
        case TypeKind::ExceptionRef:
            // Opaque catch-all handle — a pointer to the heap exception object.
            out.append("void*");
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

void CEmitter::emit_function_symbol(StringView name, String& out) {
    if (m_config.emit_main_entry && name == StringView("main")) {
        // The user's `main` becomes `main_entry`; the synthetic C `main()`
        // wrapper at the bottom of the source initializes the runtime context
        // and forwards to it.
        out.append("main_entry");
    } else {
        emit_mangled_name(name, out);
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
    }

    for (u32 b = 0; b < func->blocks.size(); b++) {
        const IRBlock* block = func->blocks[b];
        for (u32 p = 0; p < block->params.size(); p++) {
            m_value_types[block->params[p].value.id] = block->params[p].type;
            // A `ref T` / `uniq T` block param (e.g. a catch clause's exception
            // parameter) is a C pointer, so field access on it uses `->`.
            // Struct-typed block params stay values (declared `Struct vN;`).
            if (block->params[p].type && block->params[p].type->is_reference()) {
                m_pointer_values.insert(block->params[p].value.id);
            }
        }
        for (u32 i = 0; i < block->instructions.size(); i++) {
            const IRInst* inst = block->instructions[i];
            if (inst->result.is_valid() && inst->type) {
                m_value_types[inst->result.id] = inst->type;
                // A value typed `uniq Struct` / `ref Struct` is a C struct
                // pointer (emitted as `Struct*`), so field access on it uses
                // `->`. New/params are caught below; this also covers a `uniq`
                // loaded from a global (LoadPtr), returned from a call, or read
                // from a field.
                if (inst->type->kind == TypeKind::Uniq || inst->type->kind == TypeKind::Ref) {
                    Type* pointee = inst->type->base_type();
                    if (pointee && pointee->is_struct()) {
                        m_pointer_values.insert(inst->result.id);
                    }
                }
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
            if (inst->op == IROp::GlobalAddr) {
                // Address of a module global — a pointer, like GetFieldAddr.
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

// --- Module globals ---

void CEmitter::emit_global_symbol(StringView name, String& out) {
    // `g_` prefix keeps globals out of the vN / function / type namespaces.
    out.append("g_");
    emit_mangled_name(name, out);
}

const IRGlobal* CEmitter::find_global_by_offset(u32 slot_offset) {
    if (!m_module) return nullptr;
    for (u32 i = 0; i < m_module->globals.size(); i++) {
        if (m_module->globals[i].slot_offset == slot_offset) {
            return &m_module->globals[i];
        }
    }
    return nullptr;
}

// One C global per Roxy global. Zero-initialized statics — `__module_init` runs
// the real initializers (and constructors) at startup, so no C initializer here.
void CEmitter::emit_global_definitions(const IRModule* module, String& out) {
    if (module->globals.empty()) return;
    for (u32 i = 0; i < module->globals.size(); i++) {
        const IRGlobal& g = module->globals[i];
        out.append("static ");
        emit_type(g.type, out);
        out.push_back(' ');
        emit_global_symbol(g.name, out);
        out.append(";\n");
    }
    out.append("\n");
}

// Append an rx::String to the output buffer.
static inline void ap(String& out, const String& s) {
    out.append(StringView(s.data(), s.size()));
}

void CEmitter::emit_delete_slot(Type* elem, StringView slot_expr, String& out) {
    if (!elem || !elem->noncopyable()) return;  // copyable element: nothing to clean
    bool ptr_shaped = elem->kind == TypeKind::Uniq || elem->is_list()
        || elem->is_map() || elem->is_coroutine();
    if (ptr_shaped) {
        // The slot holds an owning pointer (uniq/List/Map/Coro): load and recurse.
        String ev = format("_de{}", m_delete_tmp++);
        out.append("    void* "); ap(out, ev); out.append(" = *(void**)(");
        out.append(slot_expr); out.append(");\n");
        emit_typed_delete(elem, StringView(ev.data(), ev.size()), /*free_obj=*/true, out);
    } else {
        // Inline value struct: its data lives at the slot; recurse in place.
        String cast;
        cast.append("(");
        emit_type(elem, cast);
        cast.append("*)(");
        cast.append(slot_expr);
        cast.append(")");
        emit_typed_delete(elem, StringView(cast.data(), cast.size()), /*free_obj=*/false, out);
    }
}

void CEmitter::emit_typed_delete(Type* type, StringView ptr_expr, bool free_obj, String& out) {
    if (!type) return;

    // --- List<T>: iterate noncopyable elements, free buffer, free header. ---
    if (type->is_list()) {
        if (!type->noncopyable()) return;  // copyable list: never reached via Delete
        Type* elem = type->list_info.element_type;
        u32 n = m_delete_tmp++;
        String h = format("_dl{}", n);
        out.append("    { roxy_list_header* "); ap(out, h);
        out.append(" = (roxy_list_header*)("); out.append(ptr_expr); out.append(");\n");
        out.append("    if ("); ap(out, h); out.append(") {\n");
        if (elem && elem->noncopyable()) {
            String iv = format("_di{}", n);
            String sv = format("_ds{}", n);
            out.append("    for (uint32_t "); ap(out, iv); out.append(" = 0; ");
            ap(out, iv); out.append(" < "); ap(out, h); out.append("->length; ");
            ap(out, iv); out.append("++) {\n");
            out.append("    uint32_t* "); ap(out, sv); out.append(" = "); ap(out, h);
            out.append("->elements + (size_t)"); ap(out, iv); out.append(" * ");
            ap(out, h); out.append("->element_slot_count;\n");
            emit_delete_slot(elem, StringView(sv.data(), sv.size()), out);
            out.append("    }\n");
        }
        out.append("    roxy_list_delete("); ap(out, h); out.append(");\n");
        if (free_obj) { out.append("    roxy_free("); ap(out, h); out.append(");\n"); }
        out.append("    } }\n");
        return;
    }

    // --- Map<K, V>: iterate occupied buckets, free buffers, free header. ---
    if (type->is_map()) {
        if (!type->noncopyable()) return;
        Type* kt = type->map_info.key_type;
        Type* vt = type->map_info.value_type;
        bool kc = kt && kt->noncopyable();
        bool vc = vt && vt->noncopyable();
        u32 n = m_delete_tmp++;
        String h = format("_dm{}", n);
        out.append("    { roxy_map_header* "); ap(out, h);
        out.append(" = (roxy_map_header*)("); out.append(ptr_expr); out.append(");\n");
        out.append("    if ("); ap(out, h); out.append(") {\n");
        if (kc || vc) {
            String iv = format("_di{}", n);
            out.append("    if ("); ap(out, h); out.append("->capacity > 0 && ");
            ap(out, h); out.append("->distances) {\n");
            out.append("    for (uint32_t "); ap(out, iv); out.append(" = 0; ");
            ap(out, iv); out.append(" < "); ap(out, h); out.append("->capacity; ");
            ap(out, iv); out.append("++) {\n");
            out.append("    if ("); ap(out, h); out.append("->distances["); ap(out, iv);
            out.append("] == 0) continue;\n");
            if (kc) {
                String ks = format("_dk{}", n);
                out.append("    uint32_t* "); ap(out, ks); out.append(" = "); ap(out, h);
                out.append("->keys + (size_t)"); ap(out, iv); out.append(" * ");
                ap(out, h); out.append("->key_slot_count;\n");
                emit_delete_slot(kt, StringView(ks.data(), ks.size()), out);
            }
            if (vc) {
                String vs = format("_dv{}", n);
                out.append("    uint32_t* "); ap(out, vs); out.append(" = "); ap(out, h);
                out.append("->values + (size_t)"); ap(out, iv); out.append(" * ");
                ap(out, h); out.append("->value_slot_count;\n");
                emit_delete_slot(vt, StringView(vs.data(), vs.size()), out);
            }
            out.append("    } }\n");  // close for + if(capacity)
        }
        out.append("    roxy_map_delete("); ap(out, h); out.append(");\n");
        if (free_obj) { out.append("    roxy_free("); ap(out, h); out.append(");\n"); }
        out.append("    } }\n");
        return;
    }

    // --- struct (value, or the pointee of a uniq/ref): run its destructor. ---
    // A Coro<T> is a pointer to its state struct __coro_<func>; deleting it runs
    // the generated __coro_<func>$$delete (== {struct_name}$$delete) destructor,
    // which cleans up promoted uniq/noncopyable fields, then frees the struct.
    Type* pointee = nullptr;
    if (type->is_struct()) pointee = type;
    else if (type->kind == TypeKind::Uniq || type->kind == TypeKind::Ref) pointee = type->base_type();
    else if (type->is_coroutine()) pointee = type->coro_info.generated_struct_type;
    if (pointee && pointee->is_struct()) {
        String dtor = format("{}$$delete", pointee->struct_info.name);
        if (find_function(StringView(dtor.data(), dtor.size()))) {
            out.append("    ");
            emit_function_symbol(StringView(dtor.data(), dtor.size()), out);
            out.append("((");
            emit_type(pointee, out);
            out.append("*)("); out.append(ptr_expr); out.append("));\n");
        }
    }
    if (free_obj) {
        out.append("    roxy_free("); out.append(ptr_expr); out.append(");\n");
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

    // Phase 5 follow-up: emit a `#line` directive whenever the instruction's
    // source line moves to a new line, so debugger step / error attribution
    // tracks per-statement granularity. Skipped when no source_path is set,
    // when the IR builder couldn't recover a line (synthesized lowering),
    // or when consecutive insts share the same line.
    if (!m_config.source_path.empty() && inst->source_line != 0
        && inst->source_line != m_last_emitted_source_line) {
        char buf[32];
        format_to(buf, sizeof(buf), "#line {} \"", inst->source_line);
        out.append(buf);
        out.append(StringView(m_config.source_path));
        out.append("\"\n");
        m_last_emitted_source_line = inst->source_line;
    }

    switch (inst->op) {
        // --- Constants ---
        case IROp::ConstBool: {
            out.append("    ");
            emit_value(inst->result, out);
            out.append(inst->const_data.bool_val ? " = true;\n" : " = false;\n");
            return;
        }
        case IROp::ConstInt: {
            // A void-typed constant is a return-sentinel (e.g. the coroutine
            // $$delete destructor's `return default`); it is never declared as a
            // local, so emit nothing — the void Return terminator becomes `return;`.
            if (inst->type && inst->type->kind == TypeKind::Void) return;
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
            emit_function_symbol(inst->call.func_name, out);
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
            // The post-call exception check is emitted by emit_block (after any
            // deferred move-nullify) so unwinding skips just-moved arguments.
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
            // Post-call exception check emitted by emit_block (see Call).
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
        case IROp::GlobalAddr: {
            // vN = &g_<name>;  (address of the C global backing this slot)
            out.append("    ");
            emit_value(inst->result, out);
            out.append(" = &");
            const IRGlobal* g = find_global_by_offset(inst->global_data.slot_offset);
            if (g) {
                emit_global_symbol(g->name, out);
            } else {
                // Should not happen — IR always pairs GlobalAddr with a global.
                out.append("/*missing global*/0");
            }
            out.append(";\n");
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
            // Coroutine promotion stores promoted locals into state-struct fields
            // with raw SetField, which can mismatch C++'s strict typing where the
            // regular path uses StructCopy / Nullify:
            //   - a struct-value field assigned a struct rvalue (a pointer in the
            //     C backend) must dereference the pointer;
            //   - a uniq/ref pointer field assigned a null (void*-typed) needs an
            //     explicit cast — e.g. cleanup nulling a promoted uniq field.
            {
                Type* field_type = inst->type;
                Type* val_type = get_value_type(inst->store_value);
                bool field_is_ptr = field_type &&
                    (field_type->kind == TypeKind::Uniq || field_type->kind == TypeKind::Ref);
                if (field_type && field_type->is_struct() && is_pointer_value(inst->store_value)) {
                    out.append("*");
                    emit_value(inst->store_value, out);
                } else if (field_is_ptr && val_type && val_type->kind == TypeKind::Nil) {
                    out.append("(");
                    emit_type(field_type, out);
                    out.append(")");
                    emit_value(inst->store_value, out);
                } else {
                    emit_value(inst->store_value, out);
                }
            }
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
                return;
            }
            Type* t = inst->type;
            // Heap-owning kinds get a roxy_free after their cleanup; an inline
            // value struct (deleted via its address) does not. emit_typed_delete
            // handles structs (run the destructor) and containers (iterate +
            // recurse + free buffers) recursively — the C analogue of the VM's
            // descriptor-driven delete_value.
            bool is_heap = t->kind == TypeKind::Uniq || t->is_list()
                || t->is_map() || t->is_coroutine();
            String ve;
            emit_value(inst->unary, ve);
            emit_typed_delete(t, StringView(ve.data(), ve.size()), is_heap, out);
            // Null the local after a normal scope-exit Delete so a later throw's
            // exception-path null-guard skips this already-freed owned local.
            if (m_cleanup_values.count(inst->unary.id)) {
                out.append("    "); out.append(ve); out.append(" = 0;\n");
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

        case IROp::Yield:
            // Eliminated by coroutine_lower() before the C backend runs (each
            // Yield becomes SetField/Return in the generated resume function);
            // the IR validator rejects any survivor. So this is never reached.
            return;

        case IROp::Throw: {
            // Store the exception as pending, then route to the innermost
            // enclosing try's dispatch label (or __unwind to propagate out).
            out.append("    roxy_set_pending(");
            emit_value(inst->unary, out);
            out.append(");\n    ");
            emit_exception_route(m_cur_block_id, out);
            out.append("\n");
            return;
        }

        // --- Still unsupported (Phase 4+) ---
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
            // A void-returning function always emits `return;`, even if the IR
            // carries a (void-typed) return value — e.g. coroutine $$delete
            // destructors return a void ConstInt sentinel.
            if (func->return_type && func->return_type->kind == TypeKind::Void) {
                out.append("    return;\n");
                return;
            }
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
    // Track the current block so Throw / post-call exception checks can route to
    // the innermost enclosing try's dispatch label (or __unwind).
    m_cur_block_id = block->id.id;

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

    // Move-into-call nullify ordering: a `nullify V` whose value is consumed by a
    // later call must run AFTER the call reads V (the VM treats nullify as a
    // cleanup-scope marker, not a runtime zero — emitting `V = 0` at the nullify's
    // IR position would pass null to the consuming call). We also place it BEFORE
    // the post-call exception check so unwinding cleanup skips the just-moved value.
    auto uses_as_arg = [](const IRInst* in, u32 vid) -> bool {
        if (in->op == IROp::Call || in->op == IROp::CallNative) {
            for (u32 a = 0; a < in->call.args.size(); a++)
                if (in->call.args[a].id == vid) return true;
        } else if (in->op == IROp::CallExternal) {
            for (u32 a = 0; a < in->call_external.args.size(); a++)
                if (in->call_external.args[a].id == vid) return true;
        }
        return false;
    };
    tsl::robin_set<u32> deferred_nullify_idx;          // nullify insts handled after a call
    tsl::robin_map<u32, Vector<ValueId>> flush_after;  // call inst idx -> values to null
    for (u32 i = 0; i < block->instructions.size(); i++) {
        if (block->instructions[i]->op != IROp::Nullify) continue;
        u32 vid = block->instructions[i]->unary.id;
        i32 last_use = -1;
        for (u32 j = i + 1; j < block->instructions.size(); j++) {
            if (uses_as_arg(block->instructions[j], vid)) last_use = static_cast<i32>(j);
        }
        if (last_use >= 0) {
            deferred_nullify_idx.insert(i);
            flush_after[static_cast<u32>(last_use)].push_back(block->instructions[i]->unary);
        }
    }

    // Emit instructions
    for (u32 i = 0; i < block->instructions.size(); i++) {
        if (!deferred_nullify_idx.count(i)) {
            emit_instruction(block->instructions[i], out);
        }
        // Deferred move-nullify: zero the moved value now that the call read it.
        auto fa = flush_after.find(i);
        if (fa != flush_after.end()) {
            for (u32 k = 0; k < fa->second.size(); k++) {
                out.append("    ");
                emit_value(fa->second[k], out);
                out.append(" = 0;\n");
            }
        }
        // Post-call exception check: a user-function call may throw — route a
        // pending exception to the enclosing try's dispatch or to __unwind.
        if (m_module_uses_exceptions) {
            IROp op = block->instructions[i]->op;
            if (op == IROp::Call || op == IROp::CallExternal) {
                out.append("    if (roxy_exception_pending()) ");
                emit_exception_route(m_cur_block_id, out);
                out.append("\n");
            }
        }
    }

    // Emit terminator
    emit_terminator(block, func, out);
}

// --- Function prototype ---

void CEmitter::emit_function_prototype(const IRFunction* func, String& out) {
    if (func->returns_large_struct()) {
        // Large struct returns use hidden output pointer — return void
        out.append("void");
    } else {
        emit_type(func->return_type, out);
    }
    out.push_back(' ');
    emit_function_symbol(func->name, out);
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

// --- Exception routing ---

bool CEmitter::module_uses_exceptions(const IRModule* module) {
    for (u32 f = 0; f < module->functions.size(); f++) {
        const IRFunction* func = module->functions[f];
        if (!func->exception_handlers.empty()) return true;
        for (u32 b = 0; b < func->blocks.size(); b++) {
            const IRBlock* block = func->blocks[b];
            for (u32 i = 0; i < block->instructions.size(); i++) {
                if (block->instructions[i]->op == IROp::Throw) return true;
            }
        }
    }
    return false;
}

void CEmitter::compute_exception_routing(const IRFunction* func) {
    m_try_groups.clear();
    m_block_to_group.clear();
    m_cleanup_values.clear();
    m_func_needs_unwind = false;

    // Owned locals / borrows tracked for cleanup — zero-init their pointers and
    // null them after a normal Delete so the exception-path null-guard skips
    // not-yet-created / moved / already-freed values.
    for (u32 i = 0; i < func->cleanup_info.size(); i++) {
        m_cleanup_values.insert(func->cleanup_info[i].value.id);
    }

    if (!m_module_uses_exceptions) return;

    // Group handlers sharing a try entry.
    for (u32 hi = 0; hi < func->exception_handlers.size(); hi++) {
        const IRExceptionHandler& h = func->exception_handlers[hi];
        u32 gi = UINT32_MAX;
        for (u32 g = 0; g < m_try_groups.size(); g++) {
            if (m_try_groups[g].try_entry.id == h.try_entry.id) { gi = g; break; }
        }
        if (gi == UINT32_MAX) {
            TryGroup grp;
            grp.try_entry = h.try_entry;
            for (u32 b = 0; b < h.try_body_blocks.size(); b++) {
                grp.body_blocks.insert(h.try_body_blocks[b].id);
            }
            m_try_groups.push_back(std::move(grp));
            gi = static_cast<u32>(m_try_groups.size()) - 1;
        }
        m_try_groups[gi].handler_indices.push_back(hi);
    }

    // Map each block to the innermost (smallest body) group containing it.
    for (u32 b = 0; b < func->blocks.size(); b++) {
        u32 bid = func->blocks[b]->id.id;
        u32 best = UINT32_MAX, best_size = UINT32_MAX;
        for (u32 g = 0; g < m_try_groups.size(); g++) {
            if (m_try_groups[g].body_blocks.count(bid)) {
                u32 sz = static_cast<u32>(m_try_groups[g].body_blocks.size());
                if (sz < best_size) { best_size = sz; best = g; }
            }
        }
        if (best != UINT32_MAX) m_block_to_group[bid] = best;
    }

    // For each group, the innermost OTHER group containing its try entry.
    for (u32 g = 0; g < m_try_groups.size(); g++) {
        u32 entry = m_try_groups[g].try_entry.id;
        u32 best = UINT32_MAX, best_size = UINT32_MAX;
        for (u32 o = 0; o < m_try_groups.size(); o++) {
            if (o == g) continue;
            if (m_try_groups[o].body_blocks.count(entry)) {
                u32 sz = static_cast<u32>(m_try_groups[o].body_blocks.size());
                if (sz < best_size) { best_size = sz; best = o; }
            }
        }
        m_try_groups[g].outer_group = best;
    }

    auto group_has_catch_all = [&](u32 g) -> bool {
        for (u32 idx = 0; idx < m_try_groups[g].handler_indices.size(); idx++) {
            if (func->exception_handlers[m_try_groups[g].handler_indices[idx]].type_name.empty())
                return true;
        }
        return false;
    };

    // A `__unwind` label is needed if any throw / call in an uncovered block
    // propagates, or any group's no-match path falls out of the function.
    for (u32 b = 0; b < func->blocks.size() && !m_func_needs_unwind; b++) {
        const IRBlock* block = func->blocks[b];
        if (m_block_to_group.count(block->id.id)) continue;  // covered by a group
        for (u32 i = 0; i < block->instructions.size(); i++) {
            IROp op = block->instructions[i]->op;
            if (op == IROp::Throw || op == IROp::Call || op == IROp::CallExternal) {
                m_func_needs_unwind = true;
                break;
            }
        }
    }
    for (u32 g = 0; g < m_try_groups.size() && !m_func_needs_unwind; g++) {
        if (!group_has_catch_all(g) && m_try_groups[g].outer_group == UINT32_MAX) {
            m_func_needs_unwind = true;
        }
    }
}

void CEmitter::emit_exception_route(u32 block_id, String& out) {
    auto it = m_block_to_group.find(block_id);
    if (it != m_block_to_group.end()) {
        char buf[48];
        format_to(buf, sizeof(buf), "goto __dispatch_{};", it->second);
        out.append(buf);
    } else {
        out.append("goto __unwind;");
    }
}

void CEmitter::emit_cleanup_records(const IRFunction* func, i32 body_group, String& out) {
    // LIFO: reverse creation order, matching the VM's reverse cleanup-record scan.
    for (i32 i = static_cast<i32>(func->cleanup_info.size()) - 1; i >= 0; i--) {
        const IRCleanupInfo& ci = func->cleanup_info[i];
        if (body_group >= 0 &&
            !m_try_groups[body_group].body_blocks.count(ci.start_block.id)) {
            continue;  // dispatch path: only locals created inside this try body
        }
        String ve;
        emit_value(ci.value, ve);
        out.append("    if ("); out.append(ve); out.append(") {\n");
        if (ci.kind == IRCleanupKind::RefDec) {
            out.append("    roxy_ref_dec("); out.append(ve); out.append(");\n");
        } else {
            bool is_heap = ci.type && (ci.type->kind == TypeKind::Uniq || ci.type->is_list()
                || ci.type->is_map() || ci.type->is_coroutine());
            emit_typed_delete(ci.type, StringView(ve.data(), ve.size()), is_heap, out);
        }
        out.append("    "); out.append(ve); out.append(" = 0;\n");
        out.append("    }\n");
    }
}

void CEmitter::emit_unwind_return(const IRFunction* func, String& out) {
    Type* rt = func->return_type;
    if (!rt || rt->kind == TypeKind::Void || func->returns_large_struct()) {
        out.append("    return;\n");
    } else if (rt->is_struct()) {
        out.append("    { ");
        emit_type(rt, out);
        out.append(" __z; memset(&__z, 0, sizeof(__z)); return __z; }\n");
    } else if (rt->is_enum()) {
        out.append("    return (");
        emit_type(rt, out);
        out.append(")0;\n");
    } else {
        out.append("    return 0;\n");  // int / float / bool / pointer
    }
}

void CEmitter::emit_exception_labels(const IRFunction* func, String& out) {
    if (!m_module_uses_exceptions) return;

    for (u32 g = 0; g < m_try_groups.size(); g++) {
        const TryGroup& grp = m_try_groups[g];
        char lbl[48];
        format_to(lbl, sizeof(lbl), "__dispatch_{}:;\n", g);
        out.append(lbl);
        // Clean up owned locals created inside this try body before dispatching.
        emit_cleanup_records(func, static_cast<i32>(g), out);
        // type_id if/else chain over the group's handlers (typed first, catch-all last).
        bool has_catch_all = false;
        for (u32 idx = 0; idx < grp.handler_indices.size(); idx++) {
            const IRExceptionHandler& h = func->exception_handlers[grp.handler_indices[idx]];
            char hb[32];
            format_to(hb, sizeof(hb), "block{}", h.handler_block.id);
            if (h.type_name.empty()) {
                // catch-all / finally.catch: matches unconditionally (last handler).
                out.append("    "); out.append(hb);
                out.append("_arg0 = roxy_exception_take();\n");
                out.append("    goto "); out.append(hb); out.append(";\n");
                has_catch_all = true;
                break;
            }
            out.append("    if (roxy_exception_type_id() == TYPEID_");
            emit_mangled_name(h.type_name, out);
            out.append(") { "); out.append(hb); out.append("_arg0 = (");
            emit_mangled_name(h.type_name, out);
            out.append("*)roxy_exception_take(); goto "); out.append(hb);
            out.append("; }\n");
        }
        if (!has_catch_all) {
            // No matching handler: propagate to the next-outer try or out of the frame.
            out.append("    ");
            if (grp.outer_group != UINT32_MAX) {
                char buf[48];
                format_to(buf, sizeof(buf), "goto __dispatch_{};\n", grp.outer_group);
                out.append(buf);
            } else {
                out.append("goto __unwind;\n");
            }
        }
    }

    if (m_func_needs_unwind) {
        out.append("__unwind:;\n");
        emit_cleanup_records(func, -1, out);  // whole-frame exit: all records
        emit_unwind_return(func, out);
    }
}

// --- Function emission ---

void CEmitter::emit_function(const IRFunction* func, String& out) {
    collect_value_types(func);
    compute_exception_routing(func);

    emit_function_prototype(func, out);
    out.append(" {\n");

    // Phase 5: emit a `#line N "<source>"` directive at the start of each
    // function body so debuggers attribute the body's lines to the original
    // Roxy source. `source_line == 0` means the IR builder couldn't recover
    // the source line (e.g. synthesized default ctor/dtor); skip in that
    // case to avoid emitting `#line 0`. The per-instruction tracker below
    // emits additional directives whenever the source line changes within
    // the body.
    if (!m_config.source_path.empty() && func->source_line > 0) {
        char buf[32];
        format_to(buf, sizeof(buf), "#line {} \"", func->source_line);
        out.append(buf);
        out.append(StringView(m_config.source_path));
        out.append("\"\n");
        m_last_emitted_source_line = func->source_line;
    } else {
        m_last_emitted_source_line = 0;
    }

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
                    // Pointer: StructType* v5;  (zero-init if cleanup-tracked, so
                    // the exception-path null-guard skips it before construction)
                    out.append("    ");
                    emit_type(alloc_type, out);
                    out.append("* ");
                    emit_value(inst->result, out);
                    out.append(m_cleanup_values.count(inst->result.id) ? " = 0;\n" : ";\n");
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

            if (inst->op == IROp::GlobalAddr) {
                // GlobalAddr returns a pointer to the global's storage.
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
            // Zero-init owned-local pointers tracked for exception cleanup so the
            // null-guard skips not-yet-created values.
            out.append(m_cleanup_values.count(inst->result.id) ? " = 0;\n" : ";\n");
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

    // Exception dispatch + unwind landing pads (reached only via goto).
    emit_exception_labels(func, out);

    out.append("}\n");
}

// --- Top-level emission ---

// --- Header emission helpers ---

bool CEmitter::is_pub_struct(Type* struct_type) const {
    if (!struct_type || !struct_type->is_struct()) return false;
    Decl* decl = struct_type->struct_info.decl;
    return decl && decl->struct_decl.is_pub;
}

bool CEmitter::is_pub_enum(Type* enum_type) const {
    if (!enum_type || !enum_type->is_enum()) return false;
    Decl* decl = enum_type->enum_info.decl;
    return decl && decl->enum_decl.is_pub;
}

const IRFunction* CEmitter::find_function_by_mangled(StringView mangled) {
    if (!m_module) return nullptr;
    for (u32 i = 0; i < m_module->functions.size(); i++) {
        if (m_module->functions[i]->name == mangled) {
            return m_module->functions[i];
        }
    }
    return nullptr;
}

// Splits "Foo$$bar" into ("Foo", "bar"). Returns false if no `$$` present.
static bool split_mangled(StringView name, StringView& before, StringView& after) {
    const char* data = name.data();
    u32 len = name.size();
    for (u32 i = 0; i + 1 < len; i++) {
        if (data[i] == '$' && data[i + 1] == '$') {
            before = StringView(data, i);
            after = StringView(data + i + 2, len - i - 2);
            return true;
        }
    }
    return false;
}

void CEmitter::emit_inline_method_wrapper(Type* struct_type, const MethodInfo& method,
                                          const IRFunction* func, String& out) {
    // The IRFunction holds the canonical lowered signature, but for the C++
    // wrapper we want clean parameter names from the method declaration.
    MethodDecl* method_decl = method.decl ? &method.decl->method_decl : nullptr;
    bool large_return = func->returns_large_struct();

    out.append("    ");
    if (large_return) {
        // For a large struct return the IR appends a hidden output pointer and
        // the function returns void. Skip the inline wrapper — embedders should
        // call the mangled function directly with their own out-pointer.
        out.append("// (large-struct return — call ");
        emit_mangled_name(func->name, out);
        out.append(" directly)\n");
        return;
    }
    emit_type(method.return_type, out);
    out.push_back(' ');
    out.append(method.name.data(), method.name.size());
    out.push_back('(');

    if (method_decl) {
        for (u32 i = 0; i < method_decl->params.size(); i++) {
            if (i > 0) out.append(", ");
            Type* param_type = method.param_types[i];
            emit_type(param_type, out);
            // Match the lowering convention: struct/out/inout params come in
            // through a pointer at the C ABI boundary.
            bool is_struct_param = param_type && param_type->is_struct();
            // For methods, params start at index 1 in the IRFunction (self is index 0).
            bool is_ptr_param = (i + 1) < func->param_is_ptr.size()
                && func->param_is_ptr[i + 1];
            if (is_struct_param) out.push_back('*');
            else if (is_ptr_param && param_type && !param_type->is_reference()) out.push_back('*');
            out.push_back(' ');
            out.append(method_decl->params[i].name.data(),
                       method_decl->params[i].name.size());
        }
    }
    out.append(") { ");
    if (method.return_type && !method.return_type->is_void()) {
        out.append("return ");
    }
    emit_mangled_name(func->name, out);
    out.append("(this");
    if (method_decl) {
        for (u32 i = 0; i < method_decl->params.size(); i++) {
            out.append(", ");
            out.append(method_decl->params[i].name.data(),
                       method_decl->params[i].name.size());
        }
    }
    out.append("); }\n");
}

void CEmitter::emit_make_factory(Type* struct_type, u32 type_id,
                                 const IRFunction* ctor, const IRFunction* dtor,
                                 String& out) {
    StringView struct_name = struct_type->struct_info.name;

    // Resolve the user-facing factory name. For the default ctor the name
    // ends in `$$new`; named ctors carry an extra `$$<name>` segment that
    // becomes `__<name>` in the C identifier.
    StringView before, after;
    bool has_split = split_mangled(ctor->name, before, after);
    StringView ctor_suffix;  // empty for default ctor
    if (has_split) {
        StringView dummy_before, after_inner;
        if (split_mangled(after, dummy_before, after_inner)) {
            ctor_suffix = after_inner;
        }
    }

    out.append("inline roxy::uniq<");
    emit_mangled_name(struct_name, out);
    out.append("> make_");
    emit_mangled_name(struct_name, out);
    if (!ctor_suffix.empty()) {
        out.append("__");
        out.append(ctor_suffix.data(), ctor_suffix.size());
    }
    out.push_back('(');

    // Constructor params: the IR ctor function takes self as the first param.
    // Skip it when emitting the factory's signature.
    ConstructorDecl* ctor_decl = nullptr;
    if (has_split && !ctor_suffix.empty()) {
        // Look up the named-constructor decl through struct_info.
        for (const auto& ci : struct_type->struct_info.constructors) {
            if (ci.name == ctor_suffix && ci.decl) {
                ctor_decl = &ci.decl->constructor_decl;
                break;
            }
        }
    } else {
        // Default ctor — find one with empty `name`.
        for (const auto& ci : struct_type->struct_info.constructors) {
            if (ci.name.empty() && ci.decl) {
                ctor_decl = &ci.decl->constructor_decl;
                break;
            }
        }
    }

    // Param names default to argN if no decl is available (synthesized default
    // ctor takes no args, so the loop simply produces nothing in that case).
    u32 user_param_count = ctor->params.size() > 0 ? ctor->params.size() - 1 : 0;
    for (u32 i = 0; i < user_param_count; i++) {
        if (i > 0) out.append(", ");
        Type* param_type = ctor->params[i + 1].type;
        emit_type(param_type, out);
        bool is_struct_param = param_type && param_type->is_struct();
        bool is_ptr_param = (i + 1) < ctor->param_is_ptr.size()
            && ctor->param_is_ptr[i + 1];
        if (is_struct_param) out.push_back('*');
        else if (is_ptr_param && param_type && !param_type->is_reference()) out.push_back('*');
        out.push_back(' ');
        if (ctor_decl && i < ctor_decl->params.size()) {
            out.append(ctor_decl->params[i].name.data(),
                       ctor_decl->params[i].name.size());
        } else {
            char buf[16];
            format_to(buf, sizeof(buf), "arg{}", i);
            out.append(buf);
        }
    }
    out.append(") {\n    ");
    emit_mangled_name(struct_name, out);
    out.append("* ptr = (");
    emit_mangled_name(struct_name, out);
    char tid_buf[64];
    format_to(tid_buf, sizeof(tid_buf), "*)roxy_alloc(sizeof(");
    out.append(tid_buf);
    emit_mangled_name(struct_name, out);
    format_to(tid_buf, sizeof(tid_buf), "), {});\n    ", type_id);
    out.append(tid_buf);

    emit_mangled_name(ctor->name, out);
    out.append("(ptr");
    for (u32 i = 0; i < user_param_count; i++) {
        out.append(", ");
        if (ctor_decl && i < ctor_decl->params.size()) {
            out.append(ctor_decl->params[i].name.data(),
                       ctor_decl->params[i].name.size());
        } else {
            char buf[16];
            format_to(buf, sizeof(buf), "arg{}", i);
            out.append(buf);
        }
    }
    out.append(");\n    return roxy::uniq<");
    emit_mangled_name(struct_name, out);
    out.append(">(ptr, ");
    if (dtor) {
        emit_mangled_name(dtor->name, out);
    } else {
        out.append("nullptr");
    }
    out.append(");\n}\n\n");
}

void CEmitter::emit_pub_struct_definitions(const IRModule* module, String& out) {
    // Reuse the topological sort from emit_struct_typedefs to stay consistent
    // with field-dependency ordering, but only emit pub structs.
    u32 count = module->struct_types.size();
    if (count == 0) return;

    tsl::robin_map<Type*, u32> type_to_index;
    for (u32 i = 0; i < count; i++) {
        type_to_index[module->struct_types[i]] = i;
    }

    Vector<Vector<u32>> depends_on(count);
    Vector<u32> in_degree(count, 0);
    for (u32 i = 0; i < count; i++) {
        Type* st = module->struct_types[i];
        for (u32 f = 0; f < st->struct_info.fields.size(); f++) {
            Type* ft = st->struct_info.fields[f].type;
            if (ft && ft->is_struct()) {
                auto it = type_to_index.find(ft);
                if (it != type_to_index.end()) {
                    depends_on[i].push_back(it->second);
                    in_degree[i]++;
                }
            }
        }
    }

    Vector<u32> order;
    Vector<u32> queue;
    for (u32 i = 0; i < count; i++) {
        if (in_degree[i] == 0) queue.push_back(i);
    }
    while (!queue.empty()) {
        u32 current = queue.back();
        queue.pop_back();
        order.push_back(current);
        for (u32 i = 0; i < count; i++) {
            for (u32 d = 0; d < depends_on[i].size(); d++) {
                if (depends_on[i][d] == current) {
                    in_degree[i]--;
                    if (in_degree[i] == 0) queue.push_back(i);
                }
            }
        }
    }
    if (order.size() < count) {
        for (u32 i = 0; i < count; i++) {
            bool found = false;
            for (u32 j = 0; j < order.size(); j++) {
                if (order[j] == i) { found = true; break; }
            }
            if (!found) order.push_back(i);
        }
    }

    for (u32 o = 0; o < order.size(); o++) {
        Type* struct_type = module->struct_types[order[o]];
        if (!is_pub_struct(struct_type)) continue;
        const StructTypeInfo& info = struct_type->struct_info;

        out.append("struct ");
        emit_mangled_name(info.name, out);
        out.append(" {\n");

        for (u32 f = 0; f < info.fields.size(); f++) {
            const FieldInfo& field = info.fields[f];
            out.append("    ");
            emit_type(field.type, out);
            out.push_back(' ');
            out.append(field.name.data(), field.name.size());
            out.append(";\n");
        }

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
                    out.append(variant.fields[vf].name.data(),
                               variant.fields[vf].name.size());
                    out.append("; ");
                }
                out.append("}; /* ");
                out.append(variant.case_name.data(), variant.case_name.size());
                out.append(" */\n");
            }
            out.append("    };\n");
        }

        // Inline C++ method wrappers for pub methods.
        bool any_method_emitted = false;
        for (u32 m = 0; m < info.methods.size(); m++) {
            const MethodInfo& method = info.methods[m];
            if (!method.decl) continue;  // builtin methods have no Roxy decl
            if (!method.decl->method_decl.is_pub) continue;
            StringView mangled = format_to_arena(m_alloc, "{}$${}", info.name, method.name);

            const IRFunction* func = find_function_by_mangled(mangled);
            if (!func) continue;
            if (!any_method_emitted) out.append("\n");
            any_method_emitted = true;
            emit_inline_method_wrapper(struct_type, method, func, out);
        }

        out.append("};\n\n");
    }
}

void CEmitter::emit_pub_make_factories(const IRModule* module, String& out) {
    for (u32 i = 0; i < module->struct_types.size(); i++) {
        Type* struct_type = module->struct_types[i];
        if (!is_pub_struct(struct_type)) continue;

        StringView struct_name = struct_type->struct_info.name;
        u32 type_id = 100 + i;

        // Locate the pub default destructor (used by every factory). If absent,
        // pass nullptr so `roxy::uniq` only frees without running a destructor.
        StringView dtor_mangled = format_to_arena(m_alloc, "{}$$delete", struct_name);
        const IRFunction* dtor = find_function_by_mangled(dtor_mangled);
        if (dtor && !dtor->is_pub) dtor = nullptr;

        // Iterate IRFunctions matching `Struct$$new` or `Struct$$new$$<name>`.
        for (u32 f = 0; f < module->functions.size(); f++) {
            const IRFunction* func = module->functions[f];
            if (!func->is_pub) continue;
            StringView before, after;
            if (!split_mangled(func->name, before, after)) continue;
            if (before != struct_name) continue;
            if (after == StringView("new") ||
                (after.size() > 5 && memcmp(after.data(), "new$$", 5) == 0)) {
                emit_make_factory(struct_type, type_id, func, dtor, out);
            }
        }
    }
}

// --- Top-level emission ---

void CEmitter::emit_header(const IRModule* module, String& out) {
    m_module = module;

    out.append("#pragma once\n\n");
    out.append("#include <stdint.h>\n");
    out.append("#include <stdbool.h>\n");
    out.append("#include \"roxy_rt.h\"\n\n");

    // Type IDs for pub structs (indexed by position in module->struct_types so
    // they match the IDs the .cpp uses for roxy_alloc).
    bool any_typeids = false;
    for (u32 i = 0; i < module->struct_types.size(); i++) {
        Type* st = module->struct_types[i];
        if (!is_pub_struct(st)) continue;
        if (!any_typeids) {}
        any_typeids = true;
        out.append("#define TYPEID_");
        emit_mangled_name(st->struct_info.name, out);
        char buf[16];
        format_to(buf, sizeof(buf), " {}\n", 100 + i);
        out.append(buf);
    }
    if (any_typeids) out.append("\n");

    // Pub enum typedefs
    bool any_enum = false;
    for (u32 e = 0; e < module->enum_types.size(); e++) {
        Type* enum_type = module->enum_types[e];
        if (!is_pub_enum(enum_type)) continue;
        any_enum = true;
        const EnumTypeInfo& info = enum_type->enum_info;
        out.append("typedef enum { ");
        if (info.decl) {
            const EnumDecl& enum_decl = info.decl->enum_decl;
            i64 next_value = 0;
            for (u32 v = 0; v < enum_decl.variants.size(); v++) {
                if (v > 0) out.append(", ");
                emit_mangled_name(info.name, out);
                out.push_back('_');
                out.append(enum_decl.variants[v].name.data(),
                           enum_decl.variants[v].name.size());
                i64 value = next_value;
                if (enum_decl.variants[v].value) {
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
    if (any_enum) out.append("\n");

    // Pub struct forward declarations
    bool any_struct_fwd = false;
    for (u32 i = 0; i < module->struct_types.size(); i++) {
        Type* st = module->struct_types[i];
        if (!is_pub_struct(st)) continue;
        any_struct_fwd = true;
        out.append("typedef struct ");
        emit_mangled_name(st->struct_info.name, out);
        out.push_back(' ');
        emit_mangled_name(st->struct_info.name, out);
        out.append(";\n");
    }
    if (any_struct_fwd) out.append("\n");

    // Forward declarations for all pub IRFunctions (free functions, ctors,
    // dtors, methods on pub structs). The inline method wrappers and factories
    // emitted below reference these prototypes; the embedder may also call them
    // directly. Methods on non-pub structs are skipped because their owner
    // type isn't in the header.
    bool any_proto = false;
    for (u32 i = 0; i < module->functions.size(); i++) {
        const IRFunction* func = module->functions[i];
        if (!func->is_pub) continue;
        if (m_config.emit_main_entry && func->name == StringView("main")) continue;
        StringView before, after;
        if (split_mangled(func->name, before, after)) {
            // method/ctor/dtor — owner must be a pub struct in this module
            bool owner_is_pub = false;
            for (u32 s = 0; s < module->struct_types.size(); s++) {
                Type* st = module->struct_types[s];
                if (st->struct_info.name == before && is_pub_struct(st)) {
                    owner_is_pub = true;
                    break;
                }
            }
            if (!owner_is_pub) continue;
        }
        any_proto = true;
        emit_function_prototype(func, out);
        out.append(";\n");
    }
    if (any_proto) out.append("\n");

    // Pub struct definitions with inline method wrappers
    emit_pub_struct_definitions(module, out);

    // RAII factories for pub structs (`make_<T>` / `make_<T>__<ctor>`)
    emit_pub_make_factories(module, out);
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

// Static name mapping for built-in natives (declared in roxy_rt.h, so the
// AOT pre-scan should NOT emit `extern` decls for them). Returns the C
// function name on a hit, nullptr otherwise. Both `emit_native_call` and
// `is_static_mapped_native` share this lookup.
static const char* lookup_static_native_mapping(StringView name) {
    // Built-in natives declared in roxy_rt.h: exact roxy name -> C function.
    static const tsl::robin_map<StringView, const char*> exact_mappings = {
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
        // List / Map allocation + copy (free functions, not methods). The
        // List$$<method> / Map$$<method> rows live in the method tables below,
        // so the lookup is shared with the monomorphized-instance path.
        {"list_alloc", "roxy_list_alloc"},
        {"list_copy", "roxy_list_copy"},
        {"map_alloc", "roxy_map_alloc"},
        {"map_copy", "roxy_map_copy"},
        // Internal map iteration
        {"__map_iter_capacity", "roxy_map_iter_capacity"},
        {"__map_iter_next_occupied", "roxy_map_iter_next_occupied"},
        {"__map_iter_key_at", "roxy_map_iter_key_at"},
        {"__map_iter_value_at", "roxy_map_iter_value_at"},
    };

    // Bare method name -> runtime function, shared by the unparameterized name
    // (List$$push) and every monomorphized instance (List$i32$$push) — both
    // reduce to the method after the last "$$". Single source of truth, so a new
    // List/Map method is added in exactly one place.
    static const tsl::robin_map<StringView, const char*> list_methods = {
        {"new", "roxy_list_init"},
        {"delete", "roxy_list_delete"},
        {"len", "roxy_list_len"},
        {"cap", "roxy_list_cap"},
        {"push", "roxy_list_push"},
        {"pop", "roxy_list_pop"},
        {"index", "roxy_list_get"},
        {"index_mut", "roxy_list_set"},
    };
    static const tsl::robin_map<StringView, const char*> map_methods = {
        {"new", "roxy_map_init"},
        {"delete", "roxy_map_delete"},
        {"len", "roxy_map_len"},
        {"contains", "roxy_map_contains"},
        {"get", "roxy_map_get"},
        {"insert", "roxy_map_insert"},
        {"remove", "roxy_map_remove"},
        {"clear", "roxy_map_clear"},
        {"keys", "roxy_map_keys"},
        {"values", "roxy_map_values"},
        {"index", "roxy_map_index"},
        {"index_mut", "roxy_map_index_mut"},
    };

    // Exact match
    if (auto it = exact_mappings.find(name); it != exact_mappings.end()) {
        return it->second;
    }

    // Pattern-based matches (monomorphized generic names like "List$i32$$push")
    auto suffix_after_last_dollar_dollar = [](StringView n) -> StringView {
        const char* data = n.data();
        u32 len = n.size();
        for (u32 i = len - 1; i >= 2; i--) {
            if (data[i - 1] == '$' && data[i] == '$' && i + 1 < len) {
                return StringView(data + i + 1, len - i - 1);
            }
        }
        return StringView();
    };

    // Container methods: any name starting with the container prefix and
    // containing "$$" resolves by its bare method name. Covers both the
    // unparameterized form and monomorphized instances.
    auto match_method = [&](const char* prefix,
                            const tsl::robin_map<StringView, const char*>& methods) -> const char* {
        u32 plen = static_cast<u32>(strlen(prefix));
        if (name.size() <= plen) return nullptr;
        if (StringView(name.data(), plen) != StringView(prefix)) return nullptr;
        auto it = methods.find(suffix_after_last_dollar_dollar(name));
        return it != methods.end() ? it->second : nullptr;
    };

    if (const char* c = match_method("List", list_methods)) return c;
    if (const char* c = match_method("Map", map_methods)) return c;

    // Monomorphized list_alloc / list_copy / map_alloc / map_copy
    if (name.size() > 10 && StringView(name.data(), 10) == StringView("list_alloc"))
        return "roxy_list_alloc";
    if (name.size() > 9 && StringView(name.data(), 9) == StringView("list_copy"))
        return "roxy_list_copy";
    if (name.size() > 9 && StringView(name.data(), 9) == StringView("map_alloc"))
        return "roxy_map_alloc";
    if (name.size() > 8 && StringView(name.data(), 8) == StringView("map_copy"))
        return "roxy_map_copy";

    return nullptr;
}

bool CEmitter::is_static_mapped_native(StringView name) {
    return lookup_static_native_mapping(name) != nullptr;
}

void CEmitter::emit_native_call(const IRInst* inst, String& out) {
    StringView name = inst->call.func_name;
    const char* c_func_name = lookup_static_native_mapping(name);

    if (!c_func_name) {
        // No static-table match. If the embedder registered this name via
        // `NativeRegistry`, emit a direct call to the user's C++ function —
        // the extern declaration emitted in the source preamble (see
        // `emit_extern_native_decls`) makes the linker happy whether the
        // user provided an inline header or a separately-compiled .cpp.
        // Otherwise fall back to a warning.
        if (m_config.native_registry &&
            m_config.native_registry->is_native(name)) {
            i32 idx = m_config.native_registry->get_index(name);
            const NativeFunctionEntry& entry = m_config.native_registry->get_entry(static_cast<u32>(idx));
            out.append("    ");
            if (inst->result.is_valid() && inst->type && inst->type->kind != TypeKind::Void) {
                emit_value(inst->result, out);
                out.append(" = ");
            }
            emit_mangled_name(entry.aot_symbol_name, out);
            out.push_back('(');
            for (u32 i = 0; i < inst->call.args.size(); i++) {
                if (i > 0) out.append(", ");
                emit_value(inst->call.args[i], out);
            }
            out.append(");\n");
            return;
        }

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

void CEmitter::collect_extern_native_decls(const IRModule* module) {
    if (!m_config.native_registry) return;

    for (u32 fi = 0; fi < module->functions.size(); fi++) {
        const IRFunction* func = module->functions[fi];
        // The pre-scan needs `get_value_type` for each instruction's args.
        collect_value_types(func);

        for (u32 b = 0; b < func->blocks.size(); b++) {
            const IRBlock* block = func->blocks[b];
            for (u32 j = 0; j < block->instructions.size(); j++) {
                const IRInst* inst = block->instructions[j];
                if (inst->op != IROp::CallNative) continue;

                StringView name = inst->call.func_name;
                if (is_static_mapped_native(name)) continue;
                i32 idx = m_config.native_registry->get_index(name);
                if (idx < 0) continue;

                const NativeFunctionEntry& entry =
                    m_config.native_registry->get_entry(static_cast<u32>(idx));
                if (m_extern_native_decls.find(entry.aot_symbol_name)
                    != m_extern_native_decls.end()) {
                    continue;  // already collected
                }

                ExternNativeDecl decl;
                decl.aot_symbol_name = entry.aot_symbol_name;
                decl.return_type = inst->type;
                for (u32 ai = 0; ai < inst->call.args.size(); ai++) {
                    decl.arg_types.push_back(get_value_type(inst->call.args[ai]));
                }
                m_extern_native_decls[entry.aot_symbol_name] = std::move(decl);
            }
        }
    }
}

void CEmitter::emit_extern_native_decls(String& out) {
    if (m_extern_native_decls.empty()) return;

    out.append("/* Embedder-registered native functions (declared here so the\n"
               " * AOT binary links against either an inline-defined header or a\n"
               " * separately-compiled .cpp containing the definition). */\n");
    for (auto it = m_extern_native_decls.begin();
         it != m_extern_native_decls.end(); ++it) {
        const ExternNativeDecl& decl = it->second;
        out.append("extern ");
        if (!decl.return_type || decl.return_type->kind == TypeKind::Void) {
            out.append("void");
        } else {
            emit_type(decl.return_type, out);
        }
        out.push_back(' ');
        emit_mangled_name(decl.aot_symbol_name, out);
        out.push_back('(');
        if (decl.arg_types.empty()) {
            out.append("void");
        } else {
            for (u32 i = 0; i < decl.arg_types.size(); i++) {
                if (i > 0) out.append(", ");
                Type* t = decl.arg_types[i];
                emit_type(t, out);
                // Match `emit_function_prototype`'s convention: struct args
                // are passed by pointer at the C ABI boundary.
                if (t && t->is_struct()) out.push_back('*');
            }
        }
        out.append(");\n");
    }
    out.append("\n");
}

void CEmitter::emit_source(const IRModule* module, String& output) {
    m_module = module;
    m_module_uses_exceptions = module_uses_exceptions(module);

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

    // Module-level global variable definitions.
    emit_global_definitions(module, output);

    // Pre-scan IR for `CallNative` ops that resolve through the embedder's
    // `NativeRegistry` and emit `extern` declarations. This makes the
    // generated source link against either an inline-defined header (via
    // `native_include_paths`) or a separately-compiled .cpp without
    // requiring the embedder to forward-declare the function themselves.
    collect_extern_native_decls(module);
    emit_extern_native_decls(output);

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

    // Emit the standalone C `main()` wrapper that initializes the runtime
    // context and forwards to the user's renamed `main_entry()`.
    if (m_config.emit_main_entry) {
        const IRFunction* user_main = nullptr;
        for (u32 i = 0; i < module->functions.size(); i++) {
            if (module->functions[i]->name == StringView("main")) {
                user_main = module->functions[i];
                break;
            }
        }
        if (user_main) {
            bool main_returns_void = !user_main->return_type
                || user_main->return_type->kind == TypeKind::Void;
            // Module globals: run the synthesized initializer after the ctx is
            // active (so allocations/constructors work) but before user code,
            // and the teardown after user code, before the ctx is destroyed (so
            // global destructors still have the heap). Skip when absent.
            bool has_init = find_function(StringView("__module_init", 13)) != nullptr;
            bool has_shutdown = find_function(StringView("__module_shutdown", 17)) != nullptr;
            // `roxy_rt_init` brings up the process-wide slab allocator so
            // `roxy_ctx_init` can pick it up as the default. Pairs with
            // `roxy_rt_shutdown` after the user's `main_entry()` returns.
            output.append("int main(int argc, char** argv) {\n");
            output.append("    (void)argc; (void)argv;\n");
            output.append("    roxy_rt_init();\n");
            output.append("    roxy_ctx ctx;\n");
            output.append("    roxy_ctx_init(&ctx);\n");
            output.append("    roxy_set_ctx(&ctx);\n");
            if (has_init) output.append("    __module_init();\n");
            // An exception that propagates out of main_entry is unhandled: report
            // it and exit nonzero (matches the VM's "Unhandled exception" path).
            const char* unhandled_check = m_module_uses_exceptions
                ? "    if (roxy_exception_pending()) { fprintf(stderr, \"Unhandled exception\\n\"); return 1; }\n"
                : "";
            if (main_returns_void) {
                output.append("    main_entry();\n");
                output.append(unhandled_check);
                if (has_shutdown) output.append("    __module_shutdown();\n");
                output.append("    roxy_ctx_destroy(&ctx);\n");
                output.append("    roxy_set_ctx(NULL);\n");
                output.append("    roxy_rt_shutdown();\n");
                output.append("    return 0;\n");
            } else {
                output.append("    ");
                emit_type(user_main->return_type, output);
                output.append(" result = main_entry();\n");
                output.append(unhandled_check);
                if (has_shutdown) output.append("    __module_shutdown();\n");
                output.append("    roxy_ctx_destroy(&ctx);\n");
                output.append("    roxy_set_ctx(NULL);\n");
                output.append("    roxy_rt_shutdown();\n");
                output.append("    return (int)result;\n");
            }
            output.append("}\n");
        }
    }
}

} // namespace rx

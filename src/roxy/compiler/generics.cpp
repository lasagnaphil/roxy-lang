#include "roxy/compiler/generics.hpp"

#include <cassert>
#include <cstring>

namespace rx {

// TypeSubstitution

Type* TypeSubstitution::lookup(StringView name) const {
    for (u32 i = 0; i < param_names.size(); i++) {
        if (param_names[i] == name) {
            return concrete_types[i];
        }
    }
    return nullptr;
}

// GenericInstantiator

GenericInstantiator::GenericInstantiator(BumpAllocator& allocator, TypeCache& types)
    : m_allocator(allocator)
    , m_types(types)
{
}

void GenericInstantiator::register_generic_fun(StringView name, Decl* decl) {
    m_generic_funs[name] = decl;
}

void GenericInstantiator::register_generic_struct(StringView name, Decl* decl) {
    m_generic_structs[name] = decl;
}

bool GenericInstantiator::is_generic_fun(StringView name) const {
    return m_generic_funs.find(name) != m_generic_funs.end();
}

bool GenericInstantiator::is_generic_struct(StringView name) const {
    return m_generic_structs.find(name) != m_generic_structs.end();
}

Decl* GenericInstantiator::get_generic_fun_decl(StringView name) const {
    auto it = m_generic_funs.find(name);
    if (it != m_generic_funs.end()) return it->second;
    return nullptr;
}

Decl* GenericInstantiator::get_generic_struct_decl(StringView name) const {
    auto it = m_generic_structs.find(name);
    if (it != m_generic_structs.end()) return it->second;
    return nullptr;
}

StringView GenericInstantiator::type_name_for_mangling(Type* type) {
    if (!type) return "void";

    switch (type->kind) {
        case TypeKind::Void:   return "void";
        case TypeKind::Bool:   return "bool";
        case TypeKind::I8:     return "i8";
        case TypeKind::I16:    return "i16";
        case TypeKind::I32:    return "i32";
        case TypeKind::I64:    return "i64";
        case TypeKind::U8:     return "u8";
        case TypeKind::U16:    return "u16";
        case TypeKind::U32:    return "u32";
        case TypeKind::U64:    return "u64";
        case TypeKind::F32:    return "f32";
        case TypeKind::F64:    return "f64";
        case TypeKind::String: return "string";
        case TypeKind::Struct: return type->struct_info.name;
        case TypeKind::Enum:   return type->enum_info.name;
        case TypeKind::Trait:  return type->trait_info.name;
        case TypeKind::Nil:    return "nil";
        case TypeKind::Self:   return "Self";
        case TypeKind::IntLiteral: return "i32";
        case TypeKind::Error:  return "error";
        case TypeKind::TypeParam: return type->type_param_info.name;
        case TypeKind::List: {
            // List$<elem>
            StringView prefix = "List";
            StringView elem = type_name_for_mangling(type->list_info.element_type);
            u32 total_len = prefix.size() + 1 + elem.size();
            char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, elem.data(), elem.size()); pos += elem.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Map: {
            // Map$<key>$<value>
            StringView prefix = "Map";
            StringView key_name = type_name_for_mangling(type->map_info.key_type);
            StringView val_name = type_name_for_mangling(type->map_info.value_type);
            u32 total_len = prefix.size() + 1 + key_name.size() + 1 + val_name.size();
            char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, key_name.data(), key_name.size()); pos += key_name.size();
            buf[pos++] = '$';
            memcpy(buf + pos, val_name.data(), val_name.size()); pos += val_name.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak: {
            // uniq$<inner>, ref$<inner>, weak$<inner>
            StringView prefix;
            if (type->kind == TypeKind::Uniq) prefix = "uniq";
            else if (type->kind == TypeKind::Ref) prefix = "ref";
            else prefix = "weak";
            StringView inner = type_name_for_mangling(type->ref_info.inner_type);
            u32 total_len = prefix.size() + 1 + inner.size();
            char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            buf[pos++] = '$';
            memcpy(buf + pos, inner.data(), inner.size()); pos += inner.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
        case TypeKind::Function: {
            // fun$<param1>$<param2>_ret$<return>
            StringView prefix = "fun";
            StringView ret_sep = "_ret";
            StringView ret_name = type_name_for_mangling(type->func_info.return_type);
            u32 total_len = prefix.size();
            Vector<StringView> param_names;
            for (auto* param_type : type->func_info.param_types) {
                StringView pname = type_name_for_mangling(param_type);
                param_names.push_back(pname);
                total_len += 1 + pname.size(); // '$' + name
            }
            total_len += ret_sep.size() + 1 + ret_name.size(); // _ret$<return>
            char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
            u32 pos = 0;
            memcpy(buf + pos, prefix.data(), prefix.size()); pos += prefix.size();
            for (auto& pname : param_names) {
                buf[pos++] = '$';
                memcpy(buf + pos, pname.data(), pname.size()); pos += pname.size();
            }
            memcpy(buf + pos, ret_sep.data(), ret_sep.size()); pos += ret_sep.size();
            buf[pos++] = '$';
            memcpy(buf + pos, ret_name.data(), ret_name.size()); pos += ret_name.size();
            buf[pos] = '\0';
            return StringView(buf, total_len);
        }
    }
    assert(false && "Unhandled type kind in type_name_for_mangling");
    return "unknown";
}

StringView GenericInstantiator::mangle_name(StringView base_name, Span<Type*> type_args) {
    // Calculate total length: base$arg1$arg2
    u32 total_len = base_name.size();
    for (auto* type_arg : type_args) {
        StringView arg_name = type_name_for_mangling(type_arg);
        total_len += 1 + arg_name.size(); // '$' + name
    }

    char* buf = reinterpret_cast<char*>(m_allocator.alloc_bytes(total_len + 1, 1));
    u32 pos = 0;
    memcpy(buf + pos, base_name.data(), base_name.size());
    pos += base_name.size();

    for (auto* type_arg : type_args) {
        buf[pos++] = '$';
        StringView arg_name = type_name_for_mangling(type_arg);
        memcpy(buf + pos, arg_name.data(), arg_name.size());
        pos += arg_name.size();
    }
    buf[pos] = '\0';

    return StringView(buf, total_len);
}

StringView GenericInstantiator::instantiate_fun(StringView name, Span<Type*> type_args) {
    StringView mangled = mangle_name(name, type_args);

    // Check if already instantiated
    auto it = m_fun_instance_cache.find(mangled);
    if (it != m_fun_instance_cache.end()) {
        return mangled;
    }

    // Get the template
    Decl* original = get_generic_fun_decl(name);
    assert(original && "Generic function template not found");

    FunDecl& fun_decl = original->fun_decl;
    assert(fun_decl.type_params.size() == type_args.size());

    // Build substitution
    TypeSubstitution subst;
    StringView* param_names = reinterpret_cast<StringView*>(
        m_allocator.alloc_bytes(sizeof(StringView) * type_args.size(), alignof(StringView)));
    Type** concrete_types = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * type_args.size(), alignof(Type*)));

    for (u32 i = 0; i < fun_decl.type_params.size(); i++) {
        param_names[i] = fun_decl.type_params[i].name;
        concrete_types[i] = type_args[i];
    }
    subst.param_names = Span<StringView>(param_names, type_args.size());
    subst.concrete_types = Span<Type*>(concrete_types, type_args.size());

    // Clone and substitute
    Decl* instantiated = clone_fun_decl(original, subst, mangled);

    // Create instance
    GenericFunInstance* instance = m_allocator.emplace<GenericFunInstance>();
    instance->mangled_name = mangled;
    instance->original_decl = original;
    instance->substitution = subst;
    instance->instantiated_decl = instantiated;
    instance->is_analyzed = false;

    m_all_fun_instances.push_back(instance);
    m_pending_funs.push_back(instance);
    m_fun_instance_cache[mangled] = instance;

    return mangled;
}

StringView GenericInstantiator::instantiate_struct(StringView name, Span<Type*> type_args) {
    StringView mangled = mangle_name(name, type_args);

    // Check if already instantiated
    auto it = m_struct_instance_cache.find(mangled);
    if (it != m_struct_instance_cache.end()) {
        return mangled;
    }

    // Get the template
    Decl* original = get_generic_struct_decl(name);
    assert(original && "Generic struct template not found");

    StructDecl& struct_decl = original->struct_decl;
    assert(struct_decl.type_params.size() == type_args.size());

    // Build substitution
    TypeSubstitution subst;
    StringView* param_names = reinterpret_cast<StringView*>(
        m_allocator.alloc_bytes(sizeof(StringView) * type_args.size(), alignof(StringView)));
    Type** concrete_types = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * type_args.size(), alignof(Type*)));

    for (u32 i = 0; i < struct_decl.type_params.size(); i++) {
        param_names[i] = struct_decl.type_params[i].name;
        concrete_types[i] = type_args[i];
    }
    subst.param_names = Span<StringView>(param_names, type_args.size());
    subst.concrete_types = Span<Type*>(concrete_types, type_args.size());

    // Clone and substitute
    Decl* instantiated = clone_struct_decl(original, subst, mangled);

    // Create a concrete struct type for this instantiation
    Type* concrete_type = m_types.struct_type(mangled, instantiated);

    // Create instance
    GenericStructInstance* instance = m_allocator.emplace<GenericStructInstance>();
    instance->mangled_name = mangled;
    instance->original_decl = original;
    instance->substitution = subst;
    instance->instantiated_decl = instantiated;
    instance->concrete_type = concrete_type;
    instance->is_analyzed = false;

    m_all_struct_instances.push_back(instance);
    m_pending_structs.push_back(instance);
    m_struct_instance_cache[mangled] = instance;

    return mangled;
}

bool GenericInstantiator::has_pending_funs() const {
    return !m_pending_funs.empty();
}

Vector<GenericFunInstance*> GenericInstantiator::take_pending_funs() {
    Vector<GenericFunInstance*> result;
    swap(result, m_pending_funs);
    return result;
}

bool GenericInstantiator::has_pending_structs() const {
    return !m_pending_structs.empty();
}

Vector<GenericStructInstance*> GenericInstantiator::take_pending_structs() {
    Vector<GenericStructInstance*> result;
    swap(result, m_pending_structs);
    return result;
}

GenericFunInstance* GenericInstantiator::find_fun_instance(StringView mangled_name) const {
    auto it = m_fun_instance_cache.find(mangled_name);
    if (it != m_fun_instance_cache.end()) {
        return it->second;
    }
    return nullptr;
}

GenericStructInstance* GenericInstantiator::find_struct_instance(StringView mangled_name) const {
    auto it = m_struct_instance_cache.find(mangled_name);
    if (it != m_struct_instance_cache.end()) {
        return it->second;
    }
    return nullptr;
}

void GenericInstantiator::set_fun_bounds(StringView name, ResolvedTypeParams bounds) {
    m_fun_bounds[name] = bounds;
}

void GenericInstantiator::set_struct_bounds(StringView name, ResolvedTypeParams bounds) {
    m_struct_bounds[name] = bounds;
}

const ResolvedTypeParams* GenericInstantiator::get_fun_bounds(StringView name) const {
    auto it = m_fun_bounds.find(name);
    if (it != m_fun_bounds.end()) return &it->second;
    return nullptr;
}

const ResolvedTypeParams* GenericInstantiator::get_struct_bounds(StringView name) const {
    auto it = m_struct_bounds.find(name);
    if (it != m_struct_bounds.end()) return &it->second;
    return nullptr;
}

GenericStructInstance* GenericInstantiator::find_struct_instance_by_type(Type* concrete_type) const {
    for (auto* inst : m_all_struct_instances) {
        if (inst->concrete_type == concrete_type) {
            return inst;
        }
    }
    return nullptr;
}

// AST cloning with type substitution

TypeExpr* GenericInstantiator::type_to_type_expr(Type* type, SourceLocation loc) {
    TypeExpr* result = m_allocator.emplace<TypeExpr>();
    result->loc = loc;
    result->ref_kind = RefKind::None;
    result->type_args = {};

    switch (type->kind) {
        // Primitives and named types — just use the name
        case TypeKind::Void:   result->name = "void"; break;
        case TypeKind::Bool:   result->name = "bool"; break;
        case TypeKind::I8:     result->name = "i8"; break;
        case TypeKind::I16:    result->name = "i16"; break;
        case TypeKind::I32:    result->name = "i32"; break;
        case TypeKind::I64:    result->name = "i64"; break;
        case TypeKind::U8:     result->name = "u8"; break;
        case TypeKind::U16:    result->name = "u16"; break;
        case TypeKind::U32:    result->name = "u32"; break;
        case TypeKind::U64:    result->name = "u64"; break;
        case TypeKind::F32:    result->name = "f32"; break;
        case TypeKind::F64:    result->name = "f64"; break;
        case TypeKind::String: result->name = "string"; break;
        case TypeKind::Struct: result->name = type->struct_info.name; break;
        case TypeKind::Enum:   result->name = type->enum_info.name; break;
        case TypeKind::Trait:  result->name = type->trait_info.name; break;
        case TypeKind::TypeParam: result->name = type->type_param_info.name; break;
        case TypeKind::Nil:    result->name = "nil"; break;
        case TypeKind::Self:   result->name = "Self"; break;
        case TypeKind::IntLiteral: result->name = "i32"; break;
        case TypeKind::Error:  result->name = "error"; break;

        case TypeKind::List: {
            result->name = "List";
            TypeExpr** args = reinterpret_cast<TypeExpr**>(
                m_allocator.alloc_bytes(sizeof(TypeExpr*), alignof(TypeExpr*)));
            args[0] = type_to_type_expr(type->list_info.element_type, loc);
            result->type_args = Span<TypeExpr*>(args, 1);
            break;
        }

        case TypeKind::Map: {
            result->name = "Map";
            TypeExpr** args = reinterpret_cast<TypeExpr**>(
                m_allocator.alloc_bytes(sizeof(TypeExpr*) * 2, alignof(TypeExpr*)));
            args[0] = type_to_type_expr(type->map_info.key_type, loc);
            args[1] = type_to_type_expr(type->map_info.value_type, loc);
            result->type_args = Span<TypeExpr*>(args, 2);
            break;
        }

        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak: {
            // Recursively build the inner type, then set ref_kind
            TypeExpr* inner = type_to_type_expr(type->ref_info.inner_type, loc);
            *result = *inner;
            if (type->kind == TypeKind::Uniq) result->ref_kind = RefKind::Uniq;
            else if (type->kind == TypeKind::Ref) result->ref_kind = RefKind::Ref;
            else result->ref_kind = RefKind::Weak;
            break;
        }

        case TypeKind::Function: {
            // Function types as type arguments are uncommon, but handle gracefully
            // Use the mangled name as a fallback
            result->name = type_name_for_mangling(type);
            break;
        }
    }

    return result;
}

TypeExpr* GenericInstantiator::substitute_type_expr(TypeExpr* type_expr, const TypeSubstitution& subst) {
    if (!type_expr) return nullptr;

    TypeExpr* result = m_allocator.emplace<TypeExpr>();
    *result = *type_expr;

    // Check if this type name is a type parameter that needs substitution
    if (type_expr->type_args.size() == 0) {
        Type* concrete = subst.lookup(type_expr->name);
        if (concrete) {
            // Build a proper TypeExpr that preserves compound type structure
            TypeExpr* concrete_expr = type_to_type_expr(concrete, type_expr->loc);
            // Preserve the original ref_kind if the concrete type is not already a reference
            if (type_expr->ref_kind != RefKind::None && concrete_expr->ref_kind == RefKind::None) {
                concrete_expr->ref_kind = type_expr->ref_kind;
            }
            return concrete_expr;
        }
    }

    // Handle generic type args (e.g., Box<T> -> Box<i32>, List<T> -> List<i32>)
    if (type_expr->type_args.size() > 0) {
        TypeExpr** args = reinterpret_cast<TypeExpr**>(
            m_allocator.alloc_bytes(sizeof(TypeExpr*) * type_expr->type_args.size(), alignof(TypeExpr*)));
        for (u32 i = 0; i < type_expr->type_args.size(); i++) {
            args[i] = substitute_type_expr(type_expr->type_args[i], subst);
        }
        result->type_args = Span<TypeExpr*>(args, type_expr->type_args.size());
    }

    return result;
}

Span<CallArg> GenericInstantiator::clone_call_args(Span<CallArg> args, const TypeSubstitution& subst) {
    if (args.size() == 0) return {};
    CallArg* data = reinterpret_cast<CallArg*>(
        m_allocator.alloc_bytes(sizeof(CallArg) * args.size(), alignof(CallArg)));
    for (u32 i = 0; i < args.size(); i++) {
        data[i] = args[i];
        data[i].expr = clone_expr(args[i].expr, subst);
    }
    return Span<CallArg>(data, args.size());
}

Span<FieldInit> GenericInstantiator::clone_field_inits(Span<FieldInit> fields, const TypeSubstitution& subst) {
    if (fields.size() == 0) return {};
    FieldInit* data = reinterpret_cast<FieldInit*>(
        m_allocator.alloc_bytes(sizeof(FieldInit) * fields.size(), alignof(FieldInit)));
    for (u32 i = 0; i < fields.size(); i++) {
        data[i] = fields[i];
        data[i].value = clone_expr(fields[i].value, subst);
    }
    return Span<FieldInit>(data, fields.size());
}

Expr* GenericInstantiator::clone_expr(Expr* expr, const TypeSubstitution& subst) {
    if (!expr) return nullptr;
    Expr* e = m_allocator.emplace<Expr>();
    *e = *expr;
    e->resolved_type = nullptr;

    switch (expr->kind) {
        case AstKind::ExprLiteral:
        case AstKind::ExprIdentifier:
        case AstKind::ExprThis:
            break;
        case AstKind::ExprUnary:
            e->unary.operand = clone_expr(expr->unary.operand, subst);
            break;
        case AstKind::ExprBinary:
            e->binary.left = clone_expr(expr->binary.left, subst);
            e->binary.right = clone_expr(expr->binary.right, subst);
            break;
        case AstKind::ExprTernary:
            e->ternary.condition = clone_expr(expr->ternary.condition, subst);
            e->ternary.then_expr = clone_expr(expr->ternary.then_expr, subst);
            e->ternary.else_expr = clone_expr(expr->ternary.else_expr, subst);
            break;
        case AstKind::ExprCall:
            e->call.callee = clone_expr(expr->call.callee, subst);
            e->call.arguments = clone_call_args(expr->call.arguments, subst);
            // Substitute type args on the call itself
            if (expr->call.type_args.size() > 0) {
                TypeExpr** ta = reinterpret_cast<TypeExpr**>(
                    m_allocator.alloc_bytes(sizeof(TypeExpr*) * expr->call.type_args.size(), alignof(TypeExpr*)));
                for (u32 i = 0; i < expr->call.type_args.size(); i++) {
                    ta[i] = substitute_type_expr(expr->call.type_args[i], subst);
                }
                e->call.type_args = Span<TypeExpr*>(ta, expr->call.type_args.size());
            }
            e->call.mangled_name = StringView(nullptr, 0);
            break;
        case AstKind::ExprIndex:
            e->index.object = clone_expr(expr->index.object, subst);
            e->index.index = clone_expr(expr->index.index, subst);
            break;
        case AstKind::ExprGet:
            e->get.object = clone_expr(expr->get.object, subst);
            break;
        case AstKind::ExprStaticGet:
            break;
        case AstKind::ExprAssign:
            e->assign.target = clone_expr(expr->assign.target, subst);
            e->assign.value = clone_expr(expr->assign.value, subst);
            break;
        case AstKind::ExprGrouping:
            e->grouping.expr = clone_expr(expr->grouping.expr, subst);
            break;
        case AstKind::ExprSuper:
            break;
        case AstKind::ExprStructLiteral:
            e->struct_literal.fields = clone_field_inits(expr->struct_literal.fields, subst);
            if (expr->struct_literal.type_args.size() > 0) {
                TypeExpr** ta = reinterpret_cast<TypeExpr**>(
                    m_allocator.alloc_bytes(sizeof(TypeExpr*) * expr->struct_literal.type_args.size(), alignof(TypeExpr*)));
                for (u32 i = 0; i < expr->struct_literal.type_args.size(); i++) {
                    ta[i] = substitute_type_expr(expr->struct_literal.type_args[i], subst);
                }
                e->struct_literal.type_args = Span<TypeExpr*>(ta, expr->struct_literal.type_args.size());
            }
            e->struct_literal.mangled_name = StringView(nullptr, 0);
            break;
        default:
            break;
    }
    return e;
}

Span<Decl*> GenericInstantiator::clone_decl_list(Span<Decl*> decls, const TypeSubstitution& subst) {
    if (decls.size() == 0) return {};
    Decl** data = reinterpret_cast<Decl**>(
        m_allocator.alloc_bytes(sizeof(Decl*) * decls.size(), alignof(Decl*)));
    for (u32 i = 0; i < decls.size(); i++) {
        data[i] = clone_decl(decls[i], subst);
    }
    return Span<Decl*>(data, decls.size());
}

Stmt* GenericInstantiator::clone_stmt(Stmt* stmt, const TypeSubstitution& subst) {
    if (!stmt) return nullptr;
    Stmt* s = m_allocator.emplace<Stmt>();
    *s = *stmt;

    switch (stmt->kind) {
        case AstKind::StmtExpr:
            s->expr_stmt.expr = clone_expr(stmt->expr_stmt.expr, subst);
            break;
        case AstKind::StmtBlock:
            s->block.declarations = clone_decl_list(stmt->block.declarations, subst);
            break;
        case AstKind::StmtIf:
            s->if_stmt.condition = clone_expr(stmt->if_stmt.condition, subst);
            s->if_stmt.then_branch = clone_stmt(stmt->if_stmt.then_branch, subst);
            s->if_stmt.else_branch = clone_stmt(stmt->if_stmt.else_branch, subst);
            break;
        case AstKind::StmtWhile:
            s->while_stmt.condition = clone_expr(stmt->while_stmt.condition, subst);
            s->while_stmt.body = clone_stmt(stmt->while_stmt.body, subst);
            break;
        case AstKind::StmtFor:
            s->for_stmt.initializer = clone_decl(stmt->for_stmt.initializer, subst);
            s->for_stmt.condition = clone_expr(stmt->for_stmt.condition, subst);
            s->for_stmt.increment = clone_expr(stmt->for_stmt.increment, subst);
            s->for_stmt.body = clone_stmt(stmt->for_stmt.body, subst);
            break;
        case AstKind::StmtReturn:
            s->return_stmt.value = clone_expr(stmt->return_stmt.value, subst);
            break;
        case AstKind::StmtBreak:
        case AstKind::StmtContinue:
            break;
        case AstKind::StmtDelete:
            s->delete_stmt.expr = clone_expr(stmt->delete_stmt.expr, subst);
            s->delete_stmt.arguments = clone_call_args(stmt->delete_stmt.arguments, subst);
            break;
        case AstKind::StmtWhen:
            s->when_stmt.discriminant = clone_expr(stmt->when_stmt.discriminant, subst);
            if (stmt->when_stmt.cases.size() > 0) {
                WhenCase* cases = reinterpret_cast<WhenCase*>(
                    m_allocator.alloc_bytes(sizeof(WhenCase) * stmt->when_stmt.cases.size(), alignof(WhenCase)));
                for (u32 i = 0; i < stmt->when_stmt.cases.size(); i++) {
                    cases[i] = stmt->when_stmt.cases[i];
                    cases[i].body = clone_decl_list(stmt->when_stmt.cases[i].body, subst);
                }
                s->when_stmt.cases = Span<WhenCase>(cases, stmt->when_stmt.cases.size());
            }
            s->when_stmt.else_body = clone_decl_list(stmt->when_stmt.else_body, subst);
            break;
        default:
            break;
    }
    return s;
}

Decl* GenericInstantiator::clone_decl(Decl* decl, const TypeSubstitution& subst) {
    if (!decl) return nullptr;
    Decl* d = m_allocator.emplace<Decl>();
    *d = *decl;

    switch (decl->kind) {
        case AstKind::DeclVar:
            d->var_decl.initializer = clone_expr(decl->var_decl.initializer, subst);
            d->var_decl.resolved_type = nullptr;
            // Substitute type annotation
            if (decl->var_decl.type) {
                d->var_decl.type = substitute_type_expr(decl->var_decl.type, subst);
            }
            break;
        case AstKind::StmtExpr:
            d->stmt.expr_stmt.expr = clone_expr(decl->stmt.expr_stmt.expr, subst);
            break;
        default:
            // For statement declarations embedded in Decl, clone the statement
            if (decl->kind >= AstKind::StmtExpr && decl->kind <= AstKind::StmtWhen) {
                Stmt* cloned = clone_stmt(&decl->stmt, subst);
                if (cloned) d->stmt = *cloned;
            }
            break;
    }
    return d;
}

Decl* GenericInstantiator::clone_fun_decl(Decl* original, const TypeSubstitution& subst, StringView new_name) {
    Decl* d = m_allocator.emplace<Decl>();
    *d = *original;

    FunDecl& fun_decl = d->fun_decl;
    fun_decl.name = new_name;
    fun_decl.type_params = Span<TypeParam>(); // Clear type params - this is a concrete instantiation

    // Substitute parameter types
    if (fun_decl.params.size() > 0) {
        Param* params = reinterpret_cast<Param*>(
            m_allocator.alloc_bytes(sizeof(Param) * fun_decl.params.size(), alignof(Param)));
        for (u32 i = 0; i < fun_decl.params.size(); i++) {
            params[i] = original->fun_decl.params[i];
            params[i].type = substitute_type_expr(original->fun_decl.params[i].type, subst);
        }
        fun_decl.params = Span<Param>(params, fun_decl.params.size());
    }

    // Substitute return type
    fun_decl.return_type = substitute_type_expr(original->fun_decl.return_type, subst);

    // Clone body with substitution
    fun_decl.body = clone_stmt(original->fun_decl.body, subst);

    return d;
}

Decl* GenericInstantiator::clone_struct_decl(Decl* original, const TypeSubstitution& subst, StringView new_name) {
    Decl* d = m_allocator.emplace<Decl>();
    *d = *original;

    StructDecl& struct_decl = d->struct_decl;
    struct_decl.name = new_name;
    struct_decl.type_params = Span<TypeParam>(); // Clear type params - concrete instantiation

    // Substitute field types
    if (struct_decl.fields.size() > 0) {
        FieldDecl* fields = reinterpret_cast<FieldDecl*>(
            m_allocator.alloc_bytes(sizeof(FieldDecl) * struct_decl.fields.size(), alignof(FieldDecl)));
        for (u32 i = 0; i < struct_decl.fields.size(); i++) {
            fields[i] = original->struct_decl.fields[i];
            fields[i].type = substitute_type_expr(original->struct_decl.fields[i].type, subst);
            if (fields[i].default_value) {
                fields[i].default_value = clone_expr(original->struct_decl.fields[i].default_value, subst);
            }
        }
        struct_decl.fields = Span<FieldDecl>(fields, struct_decl.fields.size());
    }

    // Clone methods with substitution
    if (struct_decl.methods.size() > 0) {
        FunDecl** methods = reinterpret_cast<FunDecl**>(
            m_allocator.alloc_bytes(sizeof(FunDecl*) * struct_decl.methods.size(), alignof(FunDecl*)));
        for (u32 i = 0; i < struct_decl.methods.size(); i++) {
            // Create a temporary Decl to clone the method
            Decl* tmp = m_allocator.emplace<Decl>();
            tmp->kind = AstKind::DeclFun;
            tmp->loc = original->loc;
            tmp->fun_decl = *original->struct_decl.methods[i];

            Decl* cloned = clone_fun_decl(tmp, subst, original->struct_decl.methods[i]->name);
            methods[i] = &cloned->fun_decl;
        }
        struct_decl.methods = Span<FunDecl*>(methods, struct_decl.methods.size());
    }

    return d;
}

} // namespace rx

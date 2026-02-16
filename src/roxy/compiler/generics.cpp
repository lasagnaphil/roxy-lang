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
        default:               return "unknown";
    }
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

    FunDecl& fd = original->fun_decl;
    assert(fd.type_params.size() == type_args.size());

    // Build substitution
    TypeSubstitution subst;
    StringView* param_names = reinterpret_cast<StringView*>(
        m_allocator.alloc_bytes(sizeof(StringView) * type_args.size(), alignof(StringView)));
    Type** concrete_types = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * type_args.size(), alignof(Type*)));

    for (u32 i = 0; i < fd.type_params.size(); i++) {
        param_names[i] = fd.type_params[i].name;
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

    StructDecl& sd = original->struct_decl;
    assert(sd.type_params.size() == type_args.size());

    // Build substitution
    TypeSubstitution subst;
    StringView* param_names = reinterpret_cast<StringView*>(
        m_allocator.alloc_bytes(sizeof(StringView) * type_args.size(), alignof(StringView)));
    Type** concrete_types = reinterpret_cast<Type**>(
        m_allocator.alloc_bytes(sizeof(Type*) * type_args.size(), alignof(Type*)));

    for (u32 i = 0; i < sd.type_params.size(); i++) {
        param_names[i] = sd.type_params[i].name;
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

// AST cloning with type substitution

TypeExpr* GenericInstantiator::substitute_type_expr(TypeExpr* type_expr, const TypeSubstitution& subst) {
    if (!type_expr) return nullptr;

    TypeExpr* result = m_allocator.emplace<TypeExpr>();
    *result = *type_expr;

    // Check if this type name is a type parameter that needs substitution
    if (type_expr->type_args.size() == 0) {
        Type* concrete = subst.lookup(type_expr->name);
        if (concrete) {
            // Replace the type name with the concrete type's name
            result->name = type_name_for_mangling(concrete);
            return result;
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

    FunDecl& fd = d->fun_decl;
    fd.name = new_name;
    fd.type_params = Span<TypeParam>(); // Clear type params - this is a concrete instantiation

    // Substitute parameter types
    if (fd.params.size() > 0) {
        Param* params = reinterpret_cast<Param*>(
            m_allocator.alloc_bytes(sizeof(Param) * fd.params.size(), alignof(Param)));
        for (u32 i = 0; i < fd.params.size(); i++) {
            params[i] = original->fun_decl.params[i];
            params[i].type = substitute_type_expr(original->fun_decl.params[i].type, subst);
        }
        fd.params = Span<Param>(params, fd.params.size());
    }

    // Substitute return type
    fd.return_type = substitute_type_expr(original->fun_decl.return_type, subst);

    // Clone body with substitution
    fd.body = clone_stmt(original->fun_decl.body, subst);

    return d;
}

Decl* GenericInstantiator::clone_struct_decl(Decl* original, const TypeSubstitution& subst, StringView new_name) {
    Decl* d = m_allocator.emplace<Decl>();
    *d = *original;

    StructDecl& sd = d->struct_decl;
    sd.name = new_name;
    sd.type_params = Span<TypeParam>(); // Clear type params - concrete instantiation

    // Substitute field types
    if (sd.fields.size() > 0) {
        FieldDecl* fields = reinterpret_cast<FieldDecl*>(
            m_allocator.alloc_bytes(sizeof(FieldDecl) * sd.fields.size(), alignof(FieldDecl)));
        for (u32 i = 0; i < sd.fields.size(); i++) {
            fields[i] = original->struct_decl.fields[i];
            fields[i].type = substitute_type_expr(original->struct_decl.fields[i].type, subst);
            if (fields[i].default_value) {
                fields[i].default_value = clone_expr(original->struct_decl.fields[i].default_value, subst);
            }
        }
        sd.fields = Span<FieldDecl>(fields, sd.fields.size());
    }

    // Clone methods with substitution
    if (sd.methods.size() > 0) {
        FunDecl** methods = reinterpret_cast<FunDecl**>(
            m_allocator.alloc_bytes(sizeof(FunDecl*) * sd.methods.size(), alignof(FunDecl*)));
        for (u32 i = 0; i < sd.methods.size(); i++) {
            // Create a temporary Decl to clone the method
            Decl* tmp = m_allocator.emplace<Decl>();
            tmp->kind = AstKind::DeclFun;
            tmp->loc = original->loc;
            tmp->fun_decl = *original->struct_decl.methods[i];

            Decl* cloned = clone_fun_decl(tmp, subst, original->struct_decl.methods[i]->name);
            methods[i] = &cloned->fun_decl;
        }
        sd.methods = Span<FunDecl*>(methods, sd.methods.size());
    }

    return d;
}

} // namespace rx

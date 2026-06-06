#include "roxy/compiler/type_checker.hpp"

namespace rx {

bool TypeChecker::is_assignable(Type* target, Type* source) const {
    if (!target || !source) return false;
    if (target->is_error() || source->is_error()) return true;
    if (target == source) return true;
    // TypeParam is assignable to itself (same name/index = same parameter)
    if (target->is_type_param() && source->is_type_param()) {
        return target->type_param_info.index == source->type_param_info.index
            && target->type_param_info.name == source->type_param_info.name;
    }
    if (source->is_nil() && target->is_reference()) return true;
    if (target->is_struct() && source->is_struct()) {
        if (is_subtype_of(source, target)) return true;
    }
    if (target->is_reference() && source->is_reference()) {
        if (target->kind == source->kind) {
            Type* target_inner = target->ref_info.inner_type;
            Type* source_inner = source->ref_info.inner_type;
            if (is_subtype_of(source_inner, target_inner)) return true;
        }
    }
    if (can_convert_ref(source, target)) return true;
    if (source->is_int_literal() && target->is_integer()) return true;
    return false;
}

bool TypeChecker::check_assignable(Type* target, Type* source, SourceLocation loc) {
    if (!target || !source) return false;
    if (target->is_error() || source->is_error()) return true;  // Don't cascade errors

    // Same type is always assignable
    if (target == source) return true;

    // TypeParam is assignable to itself (same name/index = same parameter)
    if (target->is_type_param() && source->is_type_param()) {
        if (target->type_param_info.index == source->type_param_info.index
            && target->type_param_info.name == source->type_param_info.name) {
            return true;
        }
    }

    // nil is assignable to reference types
    if (source->is_nil() && target->is_reference()) return true;

    // Struct subtyping: Child assignable to Parent (value slicing for values)
    if (target->is_struct() && source->is_struct()) {
        if (is_subtype_of(source, target)) {
            return true;
        }
    }

    // Reference subtyping: ref<Child> -> ref<Parent>, etc. (covariant)
    // Safe because struct inheritance uses prefix layout — parent fields are
    // at the same offsets in child objects, and dispatch is static.
    if (target->is_reference() && source->is_reference()) {
        if (target->kind == source->kind) {
            Type* target_inner = target->ref_info.inner_type;
            Type* source_inner = source->ref_info.inner_type;
            if (is_subtype_of(source_inner, target_inner)) {
                return true;
            }
        }
    }

    // Check reference type conversions
    if (can_convert_ref(source, target)) return true;

    // IntLiteral is assignable to any concrete integer type
    if (source->is_int_literal() && target->is_integer()) return true;

    // Strict numeric typing: no implicit conversions between numeric types
    if (target->is_numeric() && source->is_numeric() && target != source) {
        error_cannot_convert(source, target, loc, "implicitly convert");
        return false;
    }

    // Specific error messages for forbidden reference conversions
    if (source->kind == TypeKind::Ref && target->kind == TypeKind::Uniq) {
        m_reporter.error(loc, "cannot convert 'ref' to 'uniq': borrowing does not transfer ownership");
        return false;
    }
    if (source->kind == TypeKind::Weak && target->kind == TypeKind::Uniq) {
        m_reporter.error(loc, "cannot convert 'weak' to 'uniq': weak reference cannot become owner");
        return false;
    }
    if (source->kind == TypeKind::Weak && target->kind == TypeKind::Ref) {
        m_reporter.error(loc, "cannot convert 'weak' to 'ref': weak reference cannot become strong borrow");
        return false;
    }

    // nil can only be assigned to reference types
    if (source->is_nil() && !target->is_reference()) {
        m_reporter.error(loc, "'nil' can only be assigned to reference types (uniq, ref, weak)");
        return false;
    }

    auto source_str = type_string(source);
    auto target_str = type_string(target);
    m_reporter.error_fmt(loc, "cannot assign '{}' to '{}'", source_str.data(), target_str.data());
    return false;
}

bool TypeChecker::can_cast(Type* source, Type* target) {
    if (!source || !target) return false;
    if (source->is_error() || target->is_error()) return true;  // Allow error types to avoid cascading errors

    // Same type is always castable (no-op)
    if (source == target) return true;

    // Numeric to numeric: allowed
    if (source->is_numeric() && target->is_numeric()) return true;

    // Integer/float to bool: allowed
    if ((source->is_numeric()) && target->is_bool()) return true;

    // Bool to integer/float: allowed
    if (source->is_bool() && target->is_numeric()) return true;

    // String and void casts are not allowed
    if (source->kind == TypeKind::String || target->kind == TypeKind::String) return false;
    if (source->is_void() || target->is_void()) return false;

    return false;
}

bool TypeChecker::can_convert_ref(Type* from, Type* to) const {
    if (!from || !to) return false;

    // Helper to check inner type compatibility (same type or subtype)
    auto inner_compatible = [](Type* from_inner, Type* to_inner) -> bool {
        if (from_inner == to_inner) return true;
        // Covariant: Child -> Parent
        if (from_inner && to_inner && from_inner->is_struct() && to_inner->is_struct()) {
            return is_subtype_of(from_inner, to_inner);
        }
        return false;
    };

    // uniq -> ref conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Ref) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    // ref -> weak conversion
    if (from->kind == TypeKind::Ref && to->kind == TypeKind::Weak) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    // uniq -> weak conversion
    if (from->kind == TypeKind::Uniq && to->kind == TypeKind::Weak) {
        return inner_compatible(from->ref_info.inner_type, to->ref_info.inner_type);
    }

    return false;
}

bool TypeChecker::check_numeric(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_numeric()) {
        m_reporter.error(loc, "expected numeric type");
        return false;
    }
    return true;
}

bool TypeChecker::check_integer(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_integer()) {
        m_reporter.error(loc, "expected integer type");
        return false;
    }
    return true;
}

bool TypeChecker::check_boolean(Type* type, SourceLocation loc) {
    if (!type || type->is_error()) return false;
    if (!type->is_bool()) {
        m_reporter.error(loc, "expected boolean type");
        return false;
    }
    return true;
}

String TypeChecker::type_string(Type* type) {
    String str;
    type_to_string(type, str);
    str.push_back('\0');
    return str;
}

bool TypeChecker::require_types_match(Type* left, Type* right, SourceLocation loc, const char* context) {
    if (left == right) return true;

    auto left_str = type_string(left);
    auto right_str = type_string(right);
    m_reporter.error_fmt(loc, "{} requires matching types, got '{}' and '{}'",
              context, left_str.data(), right_str.data());
    return false;
}

void TypeChecker::error_cannot_convert(Type* source, Type* target, SourceLocation loc, const char* context) {
    auto source_str = type_string(source);
    auto target_str = type_string(target);
    m_reporter.error_fmt(loc, "cannot {} '{}' to '{}'",
              context, source_str.data(), target_str.data());
}

void TypeChecker::coerce_int_literal(Expr* expr, Type* target) {
    if (!expr || !target || !target->is_integer()) return;
    if (!expr->resolved_type || !expr->resolved_type->is_int_literal()) return;
    expr->resolved_type = target;
    // Recursively concretize through transparent wrappers
    if (expr->kind == AstKind::ExprGrouping)
        coerce_int_literal(expr->grouping.expr, target);
    else if (expr->kind == AstKind::ExprUnary)
        coerce_int_literal(expr->unary.operand, target);
}

}

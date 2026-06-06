#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string.hpp"
#include "roxy/shared/token.hpp"
#include "roxy/compiler/ast.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/compiler/error_reporter.hpp"

namespace rx {

// Type-relation and coercion checks: assignability, casts, reference
// conversions, numeric/integer/boolean requirements, and int-literal coercion.
// Depends only on the type system (free functions in types.hpp) and an
// ErrorReporter — no analyzer/walker state — so it stands alone.
class TypeChecker {
public:
    explicit TypeChecker(ErrorReporter& reporter) : m_reporter(reporter) {}

    bool is_assignable(Type* target, Type* source) const;
    bool check_assignable(Type* target, Type* source, SourceLocation loc);
    bool can_cast(Type* source, Type* target);
    bool can_convert_ref(Type* from, Type* to) const;
    bool check_numeric(Type* type, SourceLocation loc);
    bool check_integer(Type* type, SourceLocation loc);
    bool check_boolean(Type* type, SourceLocation loc);
    bool require_types_match(Type* left, Type* right, SourceLocation loc, const char* context);
    void error_cannot_convert(Type* source, Type* target, SourceLocation loc, const char* context);
    String type_string(Type* type);
    void coerce_int_literal(Expr* expr, Type* target);

private:
    ErrorReporter& m_reporter;
};

}

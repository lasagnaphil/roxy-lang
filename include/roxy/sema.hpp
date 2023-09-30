#pragma once

#include "roxy/stmt.hpp"
#include "roxy/type.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/core/bump_allocator.hpp"
#include "roxy/tsl/robin_map.h"

namespace rx {

class SemaEnv {
private:
    SemaEnv* m_outer;
    tsl::robin_map<std::string, Type*> m_type_map;

public:
    SemaEnv(SemaEnv* outer) : m_outer(outer) {}

    bool get_var_type(std::string_view name, Type*& type) {
        // TODO: Use a better hash map that doesn't need the std::string allocation
        auto it = m_type_map.find(std::string(name));
        if (it != m_type_map.end()) {
            type = it->second;
            return true;
        }
        else {
            if (m_outer) {
                return m_outer->get_var_type(name, type);
            }
            else {
                return false;
            }
        }
    }

    void set_var(std::string_view name, Type* type) {
        // TODO: Use a better hash map that doesn't need the std::string allocation
        m_type_map.insert({std::string(name), type});
    }
};

enum class SemaResultType {
    Ok,
    UndefinedVariable,
    IncompatibleTypes,
    CannotInferType,
    Misc,
};

// TODO: better SemaResult ADT...

struct SemaResult {
    SemaResultType type;
    Expr* expr;

    bool is_ok() const { return type == SemaResultType::Ok; }
};

class AstAllocator;

class SemaAnalyzer {
private:
    AstAllocator* m_allocator;
    Vector<SemaResult> m_errors;

public:
    SemaAnalyzer(AstAllocator* allocator) : m_allocator(allocator) {}

    Vector<SemaResult> check(BlockStmt* stmt);

private:
    SemaResult check(Stmt* stmt, SemaEnv* env);
    SemaResult get_type(Expr* expr, SemaEnv* env, Type*& type);
};

}

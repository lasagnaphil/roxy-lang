#pragma once

#include "roxy/scanner.hpp"
#include "roxy/parser.hpp"
#include "roxy/ast_visitor.hpp"
#include "roxy/opcode.hpp"
#include "roxy/chunk.hpp"
#include "roxy/module.hpp"
#include "roxy/library.hpp"

namespace rx {

enum CompileResultType {
    Ok,
    Error,
    Unimplemented,
    Unreachable
};

struct CompileResult {
    CompileResultType type;
    std::string message;
};

class FnLocalEnv {
private:
    FnLocalEnv* m_outer;

    Vector<LocalTableEntry> m_local_table;
    u32 m_param_count;

public:
    FnLocalEnv(FnLocalEnv* outer, FunctionStmt& stmt, const u8* source) : m_outer(outer) {
        init_from_locals(stmt.locals, stmt.fun_decl.params.size(), source);
    }

    FnLocalEnv(ModuleStmt& stmt, const u8* source) : m_outer(nullptr) {
        init_from_locals(stmt.locals, 0, source);
    }

    FnLocalEnv* get_outer_env() { return m_outer; }

    u16 get_local_offset(u32 local_index) const {
        return m_local_table[local_index].start;
    }

    void move_locals_to_chunk(Chunk* chunk) {
        chunk->set_locals(std::move(m_local_table), m_param_count);
    }

private:
    void init_from_locals(RelSpan<RelPtr<AstVarDecl>>& locals, u32 param_count, const u8* source) {
        u32 offset = 0;
        m_local_table.resize(locals.size());
        for (u32 i = 0; i < locals.size(); i++) {
            auto type = locals[i]->type.get();
            u32 aligned_offset = (offset + type->alignment - 1) & ~(type->alignment - 1);
            auto name = locals[i]->name.str(source);
            m_local_table[i] = {
                    (u16)((offset + 3) / 4), (u16)((type->size + 3) / 4),
                    TypeData::from_type(type, source), std::string(name)
            };
            offset = aligned_offset + type->size;
        }

        m_param_count = param_count;
    }
};

class CompilerBase :
    public StmtVisitorBase<Compiler, CompileResult>,
    public ExprVisitorBase<Compiler, CompileResult> {

protected:
    friend StmtVisitorBase;
    friend ExprVisitorBase;

    using StmtVisitorBase::visit;
    using ExprVisitorBase::visit;

    Scanner* m_scanner = nullptr;
    Module* m_cur_module = nullptr;
    Chunk* m_cur_chunk = nullptr;
    FnLocalEnv* m_cur_fn_env = nullptr;

#define COMP_TRY(EXPR) do { auto _res = EXPR; if (_res.type != CompileResultType::Ok) { return _res; } } while (false);

public:
    CompilerBase(Scanner* scanner) : m_scanner(scanner) {}

    CompileResult compile(Module& module) {
        Scanner scanner(module.m_source);

        m_scanner = &scanner;

        AstAllocator ast_allocator;

        StringInterner string_interner;
        Parser parser(&scanner, &ast_allocator, &string_interner);

        ModuleStmt* module_stmt;
        bool parse_success = parser.parse(module_stmt);

        std::string message;

        message += "Parsed output:\n";
        message += AstPrinter(scanner.source()).to_string(*module_stmt);
        message += '\n';

        if (!parse_success) return {CompileResultType::Error, message};

        SemaAnalyzer sema_analyzer(parser.get_ast_allocator(), scanner.source());
        ImportMap import_map; // empty import map
        auto sema_errors = sema_analyzer.typecheck(module_stmt, import_map);

        message += "\nAfter semantic analysis:\n";
        message += AstPrinter(scanner.source()).to_string(*module_stmt);
        message += "\n\n";

        if (!sema_errors.empty()) {
            message += "\nSema errors: ";
            message += std::to_string(sema_errors.size());
            message += '\n';

            for (auto err : sema_errors) {
                auto error_msg = err.to_error_msg(scanner.source());
                auto line = scanner.get_line(error_msg.loc);
                std::string_view str = {reinterpret_cast<const char* const>(scanner.source() + error_msg.loc.source_loc),
                                        (size_t)error_msg.loc.length};
                message += fmt::format("[line {}] Error at '{}': {}\n", line, str, error_msg.message);
            }

            return {CompileResultType::Error, message};
        }

        auto res = compile(*module_stmt, module);
        if (res.type != CompileResultType::Ok) {
            message += res.message;
            return {CompileResultType::Error, message};
        }

        return {CompileResultType::Ok, message};
    }

    CompileResult compile(ModuleStmt& stmt, Module& module) {
        m_cur_module = &module;
        m_cur_chunk = &module.chunk();

        COMP_TRY(visit(stmt));

        m_cur_module = nullptr;

        return ok();
    }

protected:
    static inline CompileResult ok() { return {CompileResultType::Ok, ""}; }

    static inline CompileResult unimplemented() {
        __debugbreak();
        return {CompileResultType::Unimplemented, "Unimplemented code"};
    }

    static inline CompileResult unreachable() {
        __debugbreak();
        return {CompileResultType::Unreachable, "Unreachable code"};
    }

    static inline CompileResult error(std::string message) {
        __debugbreak();
        return {CompileResultType::Error, std::move(message)};
    }
};

}
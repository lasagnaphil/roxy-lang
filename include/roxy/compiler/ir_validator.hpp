#pragma once

#include "roxy/core/types.hpp"
#include "roxy/compiler/ssa_ir.hpp"

namespace rx {

// IRValidator checks SSA IR for structural integrity between IR building and lowering.
// Catches malformed IR early with clear diagnostics (fail-fast on first error).
class IRValidator {
public:
    bool validate(IRModule* module);
    bool has_error() const { return m_has_error; }
    const char* error() const { return m_error; }

private:
    void report_error(const char* message);

    template<typename... Args>
    void report_error_fmt(const char* fmt, const Args&... args);

    bool validate_function(IRFunction* func);
    bool validate_block(IRFunction* func, IRBlock* block);
    bool validate_instruction(IRFunction* func, IRBlock* block, IRInst* inst);
    bool validate_terminator(IRFunction* func, IRBlock* block);
    bool validate_jump_target(IRFunction* func, const JumpTarget& target, const char* label);

    bool m_has_error = false;
    const char* m_error = nullptr;
    char m_error_buf[512];
};

}

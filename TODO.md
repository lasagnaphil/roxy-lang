# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2025-01-31

---

## Critical (Blocking / Crash Risk)

*None currently.*

---

## High Priority

- [x] **No null check after memory allocation** *(Fixed)*
  - File: `src/roxy/vm/vm.cpp:14,24`
  - Issue: `new u64[]` and `new u32[]` allocations not checked for failure
  - Fix: Used UniquePtr with `new (std::nothrow)` and proper null checks

- [x] **Visibility checking not enforced** *(Fixed)*
  - File: `src/roxy/compiler/semantic.cpp`
  - Issue: Field access ignores `is_pub` visibility modifiers
  - Fix: Added module-based visibility checking - private fields only accessible from same module

- [ ] **Constructor argument validation skipped**
  - File: `src/roxy/compiler/semantic.cpp:1104`
  - Issue: `analyze_new_expr()` doesn't validate constructor arguments
  - Fix: Validate constructor arguments match expected types

---

## Medium Priority

- [ ] **Register overflow assertion**
  - File: `src/roxy/compiler/lowering.cpp:186`
  - Issue: Functions with >255 SSA values crash (`assert(m_next_reg < 255)`)
  - Fix: Return error or implement register spilling

- [ ] **Value allocation assertion**
  - File: `src/roxy/compiler/lowering.cpp:196`
  - Issue: Unallocated values crash with assert
  - Fix: Should be caught in IR validation, not runtime

- [ ] **Reference count assertion in release builds**
  - File: `src/roxy/vm/object.cpp:57`
  - Issue: `ref_dec()` asserts `ref_count > 0` - release builds won't catch invalid decrement
  - Fix: Validate and return error instead of asserting

- [ ] **Global static type IDs not thread-safe**
  - Files: `src/roxy/vm/array.cpp`, `src/roxy/vm/string.cpp`
  - Issue: `g_array_type_id` and `g_string_type_id` use static globals
  - Fix: Move type registration to per-VM or use proper synchronization

- [ ] **Multi-register struct argument tracking**
  - File: `src/roxy/compiler/lowering.cpp:391`
  - Issue: Struct arguments spanning multiple registers not fully tracked
  - Fix: Complete register tracking for multi-register struct params

---

## Low Priority

- [ ] **Magic numbers for 16-bit range**
  - File: `src/roxy/compiler/lowering.cpp:213,265`
  - Fix: Extract `-32768`/`32767` to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:28`
  - Issue: `if (size > 1000000)` - arbitrary undocumented limit
  - Fix: Make configurable via VMConfig or define named constant

- [ ] **Float division behavior inconsistency**
  - File: `src/roxy/vm/interpreter.cpp:145-147`
  - Issue: Float div-by-zero produces IEEE 754 infinity/NaN, but integer div-by-zero errors
  - Fix: Document behavior or add optional strict mode

---

## Planned Features

- [ ] Named constructors/destructors
- [ ] Method calls (requires type system extension)
- [ ] Function overloading
- [ ] Operator overloading
- [ ] Exception handling
- [ ] LSP server for IDE support
- [ ] AOT compilation to C

---

## Code Quality Improvements

- [ ] Audit all `emit_inst()` call sites for nullptr checks
- [ ] Standardize error handling (Result type vs error_type vs nullptr)
- [ ] Add IR validation pass before lowering
- [ ] Consider extracting bytecode constants to header

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM assumption)
- [ ] Document register limit (255 values per function)
- [ ] Add inline comments explaining bytecode instruction formats

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer

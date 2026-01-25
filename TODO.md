# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2025-01-25

---

## Critical (Blocking / Crash Risk)

- [x] **Assert crash in function lookup**
  - File: `src/roxy/compiler/lowering.cpp:352`
  - Issue: `assert(false && "Function not found")` will crash in debug mode or cause undefined behavior in release (reads invalid iterator)
  - Fix: Added error reporting mechanism to BytecodeBuilder with `has_error()` / `error()` methods

- [x] **Placeholder type index for `new` expressions**
  - File: `src/roxy/compiler/lowering.cpp:540-551`
  - Issue: `u16 type_idx = 0; // Placeholder` - all `new` expressions use hardcoded type 0
  - Fix: Now reports "not yet implemented" error instead of emitting broken bytecode

- [x] **Method lookup not implemented**
  - File: `src/roxy/compiler/semantic.cpp:1088`
  - Issue: `analyze_method_call_expr()` returns error type with `// TODO: Implement proper method lookup`
  - Fix: Now reports "Method calls are not yet implemented" error

---

## High Priority

- [ ] **No null check after memory allocation**
  - File: `src/roxy/vm/vm.cpp:14,24`
  - Issue: `new u64[]` and `new u32[]` allocations not checked for failure
  - Fix: Check for nullptr and return false from `vm_init()`

- [ ] **Visibility checking not enforced**
  - File: `src/roxy/compiler/semantic.cpp:979`
  - Issue: `// TODO: Check visibility (is_pub)` - field access ignores visibility modifiers
  - Fix: Validate `is_pub` flag during field access analysis

- [ ] **Constructor argument validation skipped**
  - File: `src/roxy/compiler/semantic.cpp:1104`
  - Issue: `// TODO: Check constructor arguments` in `analyze_new_expr()`
  - Fix: Validate constructor arguments match expected types

---

## Medium Priority

- [ ] **Register overflow assertion**
  - File: `src/roxy/compiler/lowering.cpp:186`
  - Issue: `assert(m_next_reg < 255)` - functions with >255 SSA values will crash
  - Fix: Return error code or implement register spilling

- [ ] **Value allocation assertion**
  - File: `src/roxy/compiler/lowering.cpp:196`
  - Issue: `assert(it != m_value_to_reg.end())` - unallocated values crash
  - Fix: Should be caught in IR validation, not runtime assert

- [ ] **Reference count assertion in release builds**
  - File: `src/roxy/vm/object.cpp:57`
  - Issue: `assert(header->ref_count > 0)` in `ref_dec()` - release builds won't catch invalid decrement
  - Fix: Validate and return error instead of asserting

- [ ] **Global static type IDs not thread-safe**
  - Files: `src/roxy/vm/array.cpp:6-27`, `src/roxy/vm/string.cpp:8-26`
  - Issue: `g_array_type_id` and `g_string_type_id` use static globals
  - Fix: Move type registration to per-VM or use proper synchronization

- [ ] **Multi-register struct argument tracking**
  - File: `src/roxy/compiler/lowering.cpp:391`
  - Issue: `// TODO: Track that this argument uses multiple registers`
  - Fix: Complete register tracking for struct arguments spanning multiple registers

---

## Low Priority

- [ ] **Magic numbers for 16-bit range**
  - File: `src/roxy/compiler/lowering.cpp:213,265`
  - Issue: `-32768` and `32767` repeated without named constants
  - Fix: Extract to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:28`
  - Issue: `if (size > 1000000)` - arbitrary undocumented limit
  - Fix: Make configurable via VMConfig or define named constant

- [ ] **Float division doesn't set error**
  - File: `src/roxy/vm/interpreter.cpp:145-147`
  - Issue: Float division by zero produces infinity/NaN (IEEE 754 correct) but inconsistent with integer division error handling
  - Fix: Document behavior or add optional strict mode

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM assumption)
- [ ] Document register limit (255 values per function)
- [ ] Add inline comments explaining bytecode instruction formats

---

## Planned Features (Not Implemented)

These are documented in `docs/overview.md` as planned but not yet implemented:

- [ ] Heap allocation with `new`/`uniq`/`ref`/`weak`
- [ ] Named constructors/destructors
- [ ] Method calls (requires type system extension for methods on structs)
- [ ] Function overloading
- [ ] Operator overloading
- [ ] Exception handling (depends on heap allocation)
- [ ] LSP server for IDE support
- [ ] AOT compilation to C
- [ ] Large struct returns >16 bytes via hidden output pointer

---

## Code Quality Improvements

- [ ] Audit all `emit_inst()` call sites for nullptr checks
- [ ] Standardize error handling (Result type vs error_type vs nullptr)
- [ ] Add IR validation pass before lowering
- [ ] Consider extracting bytecode constants to header

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer

# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-02-01

---

## Critical (Blocking Features)

- [ ] **Cross-module function execution incomplete**
  - File: `src/roxy/vm/interpreter.cpp:460`
  - Issue: `CALL_EXTERNAL` opcode raises "External function not resolved - multi-module support not yet implemented"
  - Status: ModuleRegistry and import/export work, but VM can't execute cross-module calls
  - Impact: Multi-file programs with function calls between modules won't run

---

## High Priority

- [ ] **Register overflow assertion**
  - File: `src/roxy/compiler/lowering.cpp:208`
  - Issue: `assert(m_next_reg < 255)` - functions with >255 SSA values crash
  - Fix: Return error or implement register spilling

- [ ] **Value allocation assertion**
  - File: `src/roxy/compiler/lowering.cpp:218`
  - Issue: Unallocated values crash with assert
  - Fix: Should be caught in IR validation pass, not at lowering time

- [ ] **Reference count assertion in release builds**
  - File: `src/roxy/vm/object.cpp:81`
  - Issue: `ref_dec()` asserts `ref_count > 0` - release builds won't catch invalid decrement
  - Fix: Validate and return error instead of asserting

- [ ] **Global static type IDs not thread-safe**
  - Files: `src/roxy/vm/array.cpp:7`, `src/roxy/vm/string.cpp:9`
  - Issue: `g_array_type_id` and `g_string_type_id` use static globals with lazy initialization
  - Fix: Move type registration to per-VM or use proper synchronization

---

## Medium Priority

- [ ] **Magic numbers for 16-bit range**
  - Files: `src/roxy/compiler/lowering.cpp:235,287`
  - Fix: Extract `-32768`/`32767` to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:28`
  - Issue: `if (size > 1000000)` - arbitrary undocumented limit
  - Fix: Make configurable via VMConfig or define named constant

- [ ] **Float division behavior inconsistency**
  - File: `src/roxy/vm/interpreter.cpp:110-147`
  - Issue: Float div-by-zero produces IEEE 754 infinity/NaN, but integer div-by-zero errors
  - Fix: Document behavior or add optional strict mode

- [ ] **Local function declarations explicitly blocked**
  - File: `src/roxy/compiler/semantic.cpp:886`
  - Issue: Grammar allows local functions but semantic analysis rejects them
  - Decision needed: Support or remove from grammar

---

## Low Priority

- [ ] **Error handling standardization**
  - Issue: Mixed patterns across compiler - Result type vs error_type vs nullptr
  - Files: semantic.cpp, ir_builder.cpp, lowering.cpp
  - Fix: Standardize on single error handling pattern

- [ ] **Add IR validation pass**
  - Issue: Invalid IR can reach lowering and cause cryptic errors
  - Fix: Add validation pass between IR builder and lowering

- [ ] **Audit emit_inst() call sites**
  - File: `src/roxy/compiler/lowering.cpp`
  - Issue: Not all call sites check for nullptr returns
  - Fix: Add consistent error checking

---

## Planned Features

- [ ] Flow-sensitive typing for tagged union variant fields
- [ ] Exhaustiveness checking for when statements
- [ ] Variant constructors (`Type.Variant { ... }` syntax)
- [ ] Function overloading
- [ ] Operator overloading
- [ ] Exception handling
- [ ] LSP server for IDE support
- [ ] AOT compilation to C

---

## Code Quality Improvements

- [ ] Extract bytecode constants to dedicated header
- [ ] Add inline comments explaining bytecode instruction formats
- [ ] Standardize error message formatting across compiler
- [ ] Consider Result<T, Error> type for fallible operations

---

## Documentation Needed

- [ ] Document error type propagation pattern in semantic analysis
- [ ] Document thread-safety limitations (single VM per thread assumed)
- [ ] Document register limit (255 values per function)
- [ ] Document when/case syntax once implemented
- [ ] Add bytecode instruction format reference

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access (>5 levels)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs

---

## Recently Completed

- [x] **Tagged unions (discriminated unions)** (2026-02-01)
  - `when` clause in struct definitions for variant-specific fields
  - `when` statement for pattern matching on enum discriminants
  - Memory-efficient union layout (all variants share same storage)
  - Struct literals with explicit discriminant and variant fields
  - Phi node support for variable modifications across case branches
  - 8 E2E test cases in `tagged_unions_test.cpp`
  - Note: Flow-sensitive typing and exhaustiveness checking not yet implemented

- [x] **When statement for enums** (2026-02-01)
  - Syntax: `when expr { case A: ... case B: ... else: ... }`
  - Validates discriminant is enum type
  - Validates case names are valid enum variants
  - Detects duplicate cases
  - Compiles to comparison chain (not SWITCH opcode)
  - 12 E2E test cases in `when_test.cpp`

- [x] **Multi-register struct argument tracking** (2026-02-01)
  - Fixed bug where structs with 3-4 slots (12-16 bytes) passed as parameters weren't correctly tracked
  - Added `param_register_count` field to `BCFunction` for total registers needed by parameters
  - Fixed caller-side argument placement to use cumulative register offsets
  - Fixed callee-side parameter allocation and prologue unpacking
  - Fixed interpreter CALL/CALL_EXTERNAL to copy correct number of registers
  - 8 new E2E tests in `params_test.cpp` covering various multi-register scenarios

- [x] **Struct inheritance with `super` keyword** (2026-01-31)
  - Single inheritance for structs with static dispatch (no vtables)
  - Field inheritance: child inherits all parent fields, laid out parent-first
  - Method inheritance: child can call inherited methods, override with same name
  - `super.method()` calls parent's version of a method
  - `super()` / `super(args)` calls parent constructor
  - Constructor/destructor chaining: parent runs before child (constructors), child before parent (destructors)
  - Value slicing and covariant references supported
  - E2E tests in `inheritance_test.cpp`

- [x] **Method calls for structs** (2026-01-31)
  - Syntax: `fun StructName.method_name(params): RetType { body }`
  - Implicit `self` parameter (type `ref<StructType>`)
  - Name mangling: `StructName$method_name`
  - Methods can read/modify `self`, take parameters, return any type
  - Works with stack and heap-allocated structs
  - 8 E2E test cases in `methods_test.cpp`

- [x] Remove `new` keyword from constructor calls (2026-01-31)
  - Constructor calls now use `Type()` instead of `new Type()`
  - Heap allocation uses `uniq Type()` instead of `uniq new Type()`
  - `new` keyword kept for constructor declarations (`fun new Type()`)

- [x] Named constructors/destructors with `self` keyword (2026-01-31)

- [x] Module system with import/export (compile-time, runtime pending)

- [x] Struct field visibility checking

# Roxy Technical Debt & TODOs

This document tracks known technical debt, incomplete implementations, and planned improvements.

Last updated: 2026-02-16

---

## High Priority

*(No items — all resolved)*

---

## Medium Priority

- [ ] **Magic numbers for 16-bit range**
  - Files: `src/roxy/compiler/lowering.cpp:235,287`
  - Fix: Extract `-32768`/`32767` to `constexpr i64 IMM16_MIN/MAX`

- [ ] **Hardcoded array size limit**
  - File: `src/roxy/vm/natives.cpp:23`
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

## Type System Improvements

### Soundness Fixes

- [ ] **Covariant mutable reference subtyping is unsound**
  - File: `src/roxy/compiler/semantic.cpp` (`check_assignable`, lines 3651-3659)
  - Issue: `ref<Child>` is assignable to `ref<Parent>`, but `ref` is mutable — allows writing a `Parent` value through a reference that actually points to a `Child`, corrupting layout
  - Fix: Make reference subtyping invariant, or split into read-only `ref` (covariant) and mutable `mut_ref` (invariant)

- [x] **Struct comparison allowed without Eq trait**
  - File: `src/roxy/compiler/semantic.cpp` (`get_binary_result_type`)
  - Issue: When both operands have the same struct type, comparison is allowed even if no `Eq` trait is implemented — falls through to `return m_types.bool_type()` unconditionally
  - Fix: Restricted same-type fallback to non-struct types; structs now require trait methods

- [x] **Trait method signature validation only checks parameter count**
  - File: `src/roxy/compiler/semantic.cpp` (`validate_trait_implementations`)
  - Fixed: Full method signature validation now checks parameter types and return type with Self/TypeParam substitution via `resolve_trait_type` helper

- [x] **Name mangling collisions for compound type arguments**
  - File: `src/roxy/compiler/generics.cpp` (`type_name_for_mangling`, `type_to_type_expr`)
  - Fixed: `type_name_for_mangling` now handles all type kinds (List, Ref, Function, Trait, etc.). Added `type_to_type_expr` to build structurally correct `TypeExpr` nodes during substitution, so `List<T>` substituted with `List<i32>` resolves properly.

### Structural Improvements

- [ ] **Replace nullptr sentinel for Self type**
  - Files: `src/roxy/compiler/semantic.cpp` (`analyze_trait_method_decl`, line 1347), `include/roxy/compiler/types.hpp`
  - Issue: `Self` is encoded as `nullptr` in `TraitMethodInfo::param_types`, while trait type params use proper `TypeParam` nodes — dual encoding is fragile since `nullptr` also means "uninitialized" or "error" elsewhere
  - Fix: Add `TypeKind::Self` to the type kind enum

- [ ] **Integer literal polymorphism**
  - File: `src/roxy/compiler/semantic.cpp` (`check_assignable`)
  - Issue: `var x: i64 = 42` fails because unsuffixed `42` is always `i32` — no implicit widening
  - Fix: Add `TypeKind::IntLiteral` that unifies with any integer type during assignability checking, similar to Rust's integer inference

- [ ] **Ternary expression reports spurious errors**
  - File: `src/roxy/compiler/semantic.cpp` (`analyze_ternary_expr`, lines 2380-2385)
  - Issue: Bidirectional `check_assignable` tries `then_type <: else_type` first; if it fails, an error is reported before trying the reverse direction — succeeding in the reverse still leaves the spurious error
  - Fix: Add a non-error-reporting "probe" version of `check_assignable`

### Toward System F<:

- [ ] **Bounded quantification (`<T: Trait>`)**
  - Issue: Type parameters are unconstrained — errors in generic function bodies are only caught at monomorphization time (C++ template style), not at declaration time
  - Fix: Add trait bounds on type parameters; check generic bodies against bounds before instantiation

- [x] **Unified type environment abstraction**
  - Fixed: Introduced `TypeEnv` class consolidating `TypeCache`, `GenericInstantiator`, named/trait type registries, and `Printable` type pointer. Eliminated dual registration of named types and redundant re-analysis in `Compiler::build_ir_all()`.

### Dead Code Cleanup

- [ ] **Redundant nil-to-weak check**
  - File: `src/roxy/compiler/semantic.cpp` (`check_assignable`, line 3641)
  - Issue: `source->is_nil() && target->kind == TypeKind::Weak` is unreachable — line 3640 already handles `source->is_nil() && target->is_reference()`, and `Weak` is a reference type

- [ ] **Unreachable struct comparison block**
  - File: `src/roxy/compiler/semantic.cpp` (`get_binary_result_type`, lines 3803-3811)
  - Issue: `if (left->is_struct() && left == right)` is unreachable because `left == right` was already handled at line 3784

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
- [ ] Add bytecode instruction format reference

---

## Testing Gaps

- [ ] Test functions with >200 local variables (near register limit)
- [ ] Test deeply nested struct field access (>5 levels)
- [ ] Test error recovery in semantic analysis
- [ ] Add fuzzing for parser/lexer
- [ ] Test cross-module imports with complex dependency graphs

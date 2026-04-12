# VM Interpreter Optimization

Design plan for optimizing the bytecode VM interpreter. These are runtime optimizations independent of SSA IR passes (see `optimization.md` for compile-time optimizations).

**Current state:** The interpreter uses a switch-based dispatch loop with no special fast paths. On the quicksort benchmark (100K elements), Roxy runs at ~86ms — on par with Python (~87ms) and ~16x slower than C -O2 (~5ms).

**Benchmark profile:** The quicksort hot path is `partition`'s inner loop, which executes millions of times. Each iteration involves: list indexing (INDEX_GET_LIST), integer comparison (LE_I), conditional branch (JMP_IF_NOT), integer arithmetic (ADD_I), and frequent function calls to `swap` (CALL/RET_VOID with 4 list index operations inside).

## Phase 1: Computed Goto (Threaded Dispatch)

**Expected gain: ~25-40%**

Replace the `switch(op)` dispatch with computed goto using GCC/Clang's label-as-value extension (`&&label`).

### Current dispatch

```
top:
    decode instruction
    switch (opcode) {        // indirect branch through jump table
        case ADD_I: ...      // execute
            break;           // jump back to top
        case LE_I: ...
            break;           // jump back to top
        // 60+ cases
    }
    // check vm->running
    goto top;
```

Each instruction pays for: switch jump table lookup → handler → unconditional jump back to loop top → `vm->running` check → next decode → switch again. The CPU's branch predictor sees a single indirect branch site, making prediction poor across 60+ opcodes.

### Threaded dispatch

```cpp
static void* dispatch_table[] = {
    [0x00] = &&op_LOAD_NULL,
    [0x10] = &&op_ADD_I,
    // ... all opcodes
};

#define DISPATCH() do {             \
    instr = *pc++;                  \
    goto *dispatch_table[instr >> 24]; \
} while(0)

op_ADD_I: {
    u8 a = decode_a(instr);
    regs[a] = regs[decode_b(instr)] + regs[decode_c(instr)];
    DISPATCH();
}
```

Each handler tail-jumps directly to the next handler. The CPU sees a different indirect branch per opcode, allowing the branch predictor to specialize per-opcode (e.g., "after ADD_I, the next opcode is usually LE_I"). This is well-studied to give 1.3-1.5x speedup.

### Implementation

- Add `#if defined(__GNUC__) || defined(__clang__)` guard for computed goto; fall back to switch on MSVC.
- Move `instr` decoding into each handler (decode only needed fields).
- Eliminate the `while (vm->running)` loop — use `DISPATCH()` at the end of each handler, and `return` in HALT/RET-from-top-level.
- Keep the switch version for platforms without computed goto support.

### Files

- `src/roxy/vm/interpreter.cpp` — main dispatch loop

## Phase 2: CALL/RET Fast Path

**Expected gain: ~10-20%**

Function calls are extremely frequent in recursive algorithms. The quicksort benchmark calls `swap` ~O(N log N) times. Each CALL/RET pair currently has significant overhead.

### Current overhead in CALL

1. **Function lookup:** `vm->module->functions[func_idx].get()` — pointer chase through `UniquePtr` vector
2. **Bounds checks:** func_idx range, arg_count match, register space, local stack space
3. **Register zeroing:** `for (u32 i = 0; i < callee->register_count; i++) callee_regs[i] = 0;`
4. **Call stack push:** `vm->call_stack.push_back(new_frame)` — `Vector<CallFrame>` with capacity check

### Optimizations

**A. Replace register zeroing with `memset` (or eliminate entirely):**

The for-loop writes one u64 per iteration. Replace with:
```cpp
memset(callee_regs, 0, callee->register_count * sizeof(u64));
```

Better: if the SSA IR guarantees every register is written before read (which it should for correctly compiled code), skip zeroing entirely. Add an `#ifndef NDEBUG` guard to zero in debug builds only.

**B. Pre-allocated call stack:**

Replace `Vector<CallFrame> call_stack` with a fixed-size array:
```cpp
CallFrame* call_stack;     // malloc'd array of max_call_depth entries
u32 call_stack_size;       // current depth
```

This eliminates the `ensure_capacity` check and potential reallocation on every push. `push_back` becomes `call_stack[call_stack_size++] = frame;` and `pop_back` becomes `--call_stack_size;`.

**C. Cache callee function pointer:**

Pre-compute a flat `BCFunction**` array at module load time to avoid the `UniquePtr::get()` indirection:
```cpp
// In BCModule or RoxyVM:
BCFunction** function_ptrs;  // flat array, built at load time
```

Then CALL becomes `const BCFunction* callee = vm->function_ptrs[func_idx];` (single array index instead of vector index + unique_ptr dereference).

**D. Gate bounds checks on debug mode:**

The arg_count check, func_idx range check, and register space check protect against compiler bugs, not user errors. Gate them behind `assert()` or `#ifndef NDEBUG`.

### Files

- `src/roxy/vm/interpreter.cpp` — CALL/RET handlers
- `include/roxy/vm/vm.hpp` — CallFrame, RoxyVM (call stack type change)
- `src/roxy/vm/vm.cpp` — VM initialization

## Phase 3: Fused Compare-and-Branch Opcodes

**Expected gain: ~5-15%**

The compiler currently generates separate compare + conditional branch instructions:
```
LE_I  r5, r3, r4        ; r5 = (a <= b)
JMP_IF_NOT r5, +offset  ; if (!r5) skip
```

Fusing these into a single instruction halves the dispatch overhead for the compare+branch pattern, which is the single most common pattern in loops.

### New opcodes

```
JMP_IF_LT_I  b, c, offset   ; if (regs[b] <  regs[c]) pc += offset
JMP_IF_LE_I  b, c, offset   ; if (regs[b] <= regs[c]) pc += offset
JMP_IF_GT_I  b, c, offset   ; if (regs[b] >  regs[c]) pc += offset
JMP_IF_GE_I  b, c, offset   ; if (regs[b] >= regs[c]) pc += offset
JMP_IF_EQ_I  b, c, offset   ; if (regs[b] == regs[c]) pc += offset
JMP_IF_NE_I  b, c, offset   ; if (regs[b] != regs[c]) pc += offset
```

Encoding: `[opcode:8][offset_high:8][src1:8][src2:8]` or a new format that packs a wider signed offset. One option is to reuse the ABI format with source registers packed: `[opcode:8][src1:4|src2:4][offset:16]` — but this limits registers to 0-15, which may be acceptable since comparisons usually involve low-numbered registers.

Alternatively, use a two-word encoding:
```
Word 1: [opcode:8][pad:8][src1:8][src2:8]
Word 2: [offset:32]
```

This costs one extra word but keeps the full 8-bit register range and gives a 32-bit offset.

### Implementation

1. **Lowering pass:** After lowering, add a peephole pass that scans for `CMP + JMP_IF/JMP_IF_NOT` pairs and fuses them into a single fused opcode.
2. **Interpreter:** Add handlers for each fused opcode.
3. **Fallback:** If the comparison result is used elsewhere (not just by the branch), don't fuse.

### Files

- `include/roxy/vm/bytecode.hpp` — new opcode definitions
- `src/roxy/compiler/lowering.cpp` — peephole fusion pass
- `src/roxy/vm/interpreter.cpp` — fused opcode handlers

## Phase 4: List Indexing Fast Path

**Expected gain: ~5-10%**

`INDEX_GET_LIST` and `INDEX_SET_LIST` are the most frequent non-arithmetic operations in array-heavy code. The current implementation has unnecessary overhead for the common case (inline primitives).

### Current overhead

```cpp
case Opcode::INDEX_GET_LIST: {
    void* lst_ptr = reg_as_ptr(regs[b]);
    if (!lst_ptr) { ... }                          // null check
    i64 idx = reg_as_i64(regs[c]);
    ListHeader* header = get_list_header(lst_ptr);
    if (idx < 0 || (u64)idx >= header->length) { ... } // bounds check (i64 comparison)
    if (header->element_is_inline) {               // branch on element type
        u64 val = 0;
        memcpy(&val, list_element_ptr(header, (u32)idx),
               sizeof(u32) * header->element_slot_count);  // memcpy for 4 bytes!
        regs[a] = val;
    } else { ... }
}
```

For `List<i32>`, this does a `memcpy` of 4 bytes and branches on `element_is_inline` on every access.

### Optimizations

**A. Specialize for common slot counts:**

```cpp
if (header->element_slot_count == 1) {
    regs[a] = static_cast<u64>(header->elements[idx]);
} else if (header->element_slot_count == 2) {
    u32* p = header->elements + idx * 2;
    regs[a] = static_cast<u64>(p[0]) | (static_cast<u64>(p[1]) << 32);
} else {
    // general memcpy path
}
```

**B. Use unsigned index comparison:**

Replace the signed `i64 idx` check with an unsigned trick:
```cpp
u64 idx = regs[c];  // treat as unsigned
if (idx >= header->length) { error; }
```

This handles negative indices (which become large unsigned values) with a single comparison instead of two.

### Files

- `src/roxy/vm/interpreter.cpp` — INDEX_GET_LIST / INDEX_SET_LIST handlers

## Phase 5: Minor Optimizations

### A. Eliminate `vm->running` loop condition

Replace `while (vm->running)` with `for (;;)`. The only places that set `running = false` are RET-from-top-level and HALT, both of which already `return`. This saves one branch per instruction dispatch.

### B. ~~`__attribute__((flatten))` on interpret()~~ (rejected)

Tested and rejected: inlining all callees into the dispatch loop increased icache pressure, resulting in worse performance overall.

### C. Profile-Guided Optimization (PGO)

Build with PGO for the final binary:
```bash
# Instrumented build
cmake .. -G Ninja -DCMAKE_CXX_FLAGS="-fprofile-generate"
ninja && ./roxy benchmarks/quicksort/quicksort.roxy

# Optimized build
cmake .. -G Ninja -DCMAKE_CXX_FLAGS="-fprofile-use"
ninja
```

This lets the compiler optimize branch layout and inlining decisions based on actual execution profiles. Typical gain: 10-20% on interpreter-heavy code.

## Summary

| Phase | Optimization | Expected Gain | Effort | Scope |
|-------|-------------|---------------|--------|-------|
| 1 | Computed goto | 25-40% | Medium | interpreter.cpp |
| 2 | CALL/RET fast path | 10-20% | Low-Medium | interpreter.cpp, vm.hpp |
| 3 | Fused compare+branch | 5-15% | Medium | bytecode.hpp, lowering.cpp, interpreter.cpp |
| 4 | List index fast path | 5-10% | Low | interpreter.cpp |
| 5 | Minor optimizations | 1-5% each | Trivial-Low | interpreter.cpp, build system |

**Target:** Bring quicksort from ~86ms to ~40-55ms (2x faster than Python, ~8-10x of C).

---

## Phase 6: Immediate-Operand Arithmetic

**Expected gain: ~5-15%**

The most common arithmetic pattern in loops is increment: `i = i + 1`. This currently requires 2 instructions and a temporary register:

```
LOAD_INT  tmp, 1
ADD_I     i, i, tmp
```

A fused `ADDI` instruction with a signed 8-bit immediate eliminates the LOAD_INT and frees a register:

```
ADDI  dst, src, +1    ; [opcode:8][dst:8][src:8][imm8:8]
```

This fits naturally in the ABC format where the `c` field is reinterpreted as a signed i8 (range -128..+127). Covers the vast majority of loop increments, decrements, and small constant arithmetic.

### New opcodes

```
ADDI  dst, src, imm8   ; regs[dst] = regs[src] + (i8)imm8
SUBI  dst, src, imm8   ; regs[dst] = regs[src] - (i8)imm8
MULI  dst, src, imm8   ; regs[dst] = regs[src] * (i8)imm8
```

### Implementation

1. **Peephole pass:** Extend `fuse_compare_branch()` (or add a new pass) to scan for `LOAD_INT imm; BINOP dst, src, tmp` pairs where `imm` fits in i8 and `tmp` is dead after the binary op.
2. **Interpreter:** Add handlers for each fused opcode — single decode + immediate arithmetic.
3. **Lowering:** Alternatively, emit ADDI directly when the IR operation has a constant operand in the i8 range.

### Files

- `include/roxy/vm/bytecode.hpp` — new opcode definitions
- `src/roxy/compiler/lowering.cpp` — peephole fusion or direct emission
- `src/roxy/vm/interpreter.cpp` — fused opcode handlers

## Phase 7: Inline Trivial Native Calls

**Expected gain: ~5-10% for list/map-heavy code**

`list_len()`, `list_cap()`, and `string_length()` are trivial field loads wrapped in native function calls. Each `CALL_NATIVE` has overhead: function pointer lookup, indirect call, register index manipulation inside the native, and return path.

These should be dedicated opcodes that compile to a single pointer dereference + field load:

### New opcodes

```
LIST_LEN   dst, list_reg    ; regs[dst] = get_list_header(regs[list_reg])->length
LIST_CAP   dst, list_reg    ; regs[dst] = get_list_header(regs[list_reg])->capacity
STR_LEN    dst, str_reg     ; regs[dst] = get_string_header(regs[str_reg])->length
```

Each is 2-3 machine instructions vs. an indirect call through the native function table. In list-heavy loops that check `i < scores.len()` on every iteration, this eliminates the native call overhead entirely.

### Implementation

1. **Lowering:** When emitting a `CALL_NATIVE` for a known trivial native function (identified by name or index), emit the specialized opcode instead.
2. **Interpreter:** Add handlers — each is a single pointer cast + field load.
3. **Native registry:** Mark certain native functions as "inlineable" so the lowering pass can identify them.

### Files

- `include/roxy/vm/bytecode.hpp` — new opcode definitions
- `src/roxy/compiler/lowering.cpp` — specialized emission for known natives
- `src/roxy/vm/interpreter.cpp` — new opcode handlers

## Phase 8: String Constant Interning

**Expected gain: ~5-20% for string-heavy code**

Every `LOAD_CONST` with a String type calls `string_alloc()` which does `object_alloc` + `memcpy` — a heap allocation per load. In a loop that uses an f-string with a constant prefix, the same string literal is re-allocated every iteration.

### Current behavior

```cpp
static u64 load_constant(RoxyVM* vm, const BCFunction* func, u16 index) {
    const BCConstant& c = func->constants[index];
    // ...
    case BCConstant::String:
        return reg_from_ptr(string_alloc(vm, c.as_string.data, c.as_string.length));
}
```

### Optimization

Pre-allocate string constants at module load time. Store the resulting pointers in a flat array on the VM:

```cpp
// At module load (in vm_load_module):
for each string constant across all functions:
    vm->string_constants[i] = string_alloc(vm, data, length);

// In LOAD_CONST handler:
case BCConstant::String:
    return reg_from_ptr(vm->string_constants[interned_index]);
```

This eliminates all runtime allocation for string literals. The interned strings are kept alive for the module's lifetime and freed at module unload.

### Implementation

1. **Module loading:** Scan all functions' constant pools, collect string constants, allocate them once, and store pointers in a flat array.
2. **BCConstant:** Add an `interned_index` field for string constants that maps to the pre-allocated array.
3. **Ownership:** Interned strings are owned by the VM/module and must not be freed by `DEL_OBJ`. Either mark them with a flag in the ObjectHeader or use a separate ref-count scheme.

### Files

- `src/roxy/vm/vm.cpp` — pre-allocation in `vm_load_module`
- `include/roxy/vm/vm.hpp` — `string_constants` array
- `src/roxy/vm/interpreter.cpp` — update `load_constant()` for String case

## Phase 9: Fused Compare-and-Branch for Floats

**Expected gain: ~3-8% for float-heavy code**

The peephole pass in `fuse_compare_branch()` only fuses integer comparisons. Float comparisons (`EQ_F`/`LT_F`/`LE_F`/etc. and f64 variants) still generate separate compare + branch instructions. For game engine code with physics and vector math, float comparisons in loops are common.

### New opcodes (12 total)

```
JMP_IF_LT_F, JMP_IF_LE_F, JMP_IF_GT_F, JMP_IF_GE_F, JMP_IF_EQ_F, JMP_IF_NE_F
JMP_IF_LT_D, JMP_IF_LE_D, JMP_IF_GT_D, JMP_IF_GE_D, JMP_IF_EQ_D, JMP_IF_NE_D
```

Same two-word encoding as the integer variants: `[opcode:8][_:8][src1:8][src2:8] + [offset:32]`.

### Implementation

Extend the existing `fuse_compare_branch()` switch statement to match `EQ_F`–`GE_F` and `EQ_D`–`GE_D` opcodes, emitting the corresponding fused opcode. The interpreter handlers follow the same pattern as the integer variants but use `reg_as_f32()`/`reg_as_f64()` for operand decoding.

### Files

- `include/roxy/vm/bytecode.hpp` — new opcode definitions
- `src/roxy/compiler/lowering.cpp` — extend peephole pass
- `src/roxy/vm/interpreter.cpp` — new handlers

## Phase 10: Tail Call Optimization

**Expected gain: ~10-25% for recursive algorithms**

Recursive calls in tail position (where the return value is directly returned) can reuse the current call frame instead of pushing a new one. This eliminates frame push/pop, register window allocation/deallocation, and local stack management for each recursive call. Especially valuable for quicksort-style recursive algorithms.

### Detection

At lowering time, identify a `CALL` immediately followed by `RET` where the CALL's destination register matches the RET's source register, and no cleanup (destructors, ref-dec) is needed between them.

### New opcode

```
TAIL_CALL  func_idx, arg_count   ; [opcode:8][_:8][func_idx:8][arg_count:8]
```

### Semantics

1. Copy arguments to the start of the current register window (in-place, handling overlap with `memmove` if needed).
2. Reset local stack to current frame's base.
3. Update `func`, `pc` to point to the callee.
4. Continue execution without pushing or popping a frame.

### Constraints

- Only applicable for self-recursion or calls to functions with `register_count <= current register_count` (to avoid growing the register window).
- Cannot be used if there are cleanup records active at the call site (e.g., destructors for `uniq` locals).
- The callee's `local_stack_slots` must not exceed the current allocation.

### Files

- `include/roxy/vm/bytecode.hpp` — new opcode definition
- `src/roxy/compiler/lowering.cpp` — detect tail-call pattern, emit TAIL_CALL
- `src/roxy/vm/interpreter.cpp` — TAIL_CALL handler

## Phase 11: Branch Prediction Hints

**Expected gain: ~2-5%**

Error paths (null checks, bounds checks, overflow checks) are cold but currently given equal weight by the branch predictor. Using `__builtin_expect` pushes error-handling code out of the hot path, improving instruction cache locality and branch prediction.

### Application sites

```cpp
OP(INDEX_GET_LIST) {
    void* lst_ptr = reg_as_ptr(regs[b]);
    if (__builtin_expect(!lst_ptr, 0)) {         // unlikely: null list
        vm->error = "list index: null list reference";
        return false;
    }
    u64 idx = regs[decode_c(instr)];
    if (__builtin_expect(idx >= header->length, 0)) {  // unlikely: out of bounds
        vm->error = "List index out of bounds";
        return false;
    }
    // ... fast path continues linearly
}
```

Apply systematically to:
- Null pointer checks (INDEX_GET_LIST, INDEX_SET_LIST, INDEX_GET_MAP, INDEX_SET_MAP, DEL_OBJ)
- Bounds checks (list indexing)
- Division-by-zero checks (DIV_I, MOD_I)
- Register/stack overflow checks (CALL)
- Call stack depth checks (CALL)

### Files

- `src/roxy/vm/interpreter.cpp` — add `__builtin_expect` to error branches

## Phase 12: Local Stack Base Caching

**Expected gain: ~1-3%**

`STACK_ADDR`, `SPILL_REG`, and `RELOAD_REG` all compute `vm->local_stack.get() + frame->local_stack_base` on every access. This involves loading the `UniquePtr` internal pointer and the frame's base offset.

### Optimization

Cache this as a local variable alongside `regs` and `pc`:

```cpp
u32* local_base = vm->local_stack.get() + frame->local_stack_base;
// Update on CALL/RET along with pc, regs, func
```

Then `STACK_ADDR` becomes:
```cpp
regs[decode_a(instr)] = reg_from_ptr(local_base + decode_imm16(instr));
```

And `SPILL_REG`/`RELOAD_REG` avoid recomputing the base:
```cpp
u32* addr = local_base + slot_offset;  // instead of vm->local_stack.get() + frame->local_stack_base + slot_offset
```

### Files

- `src/roxy/vm/interpreter.cpp` — add `local_base` local, update on CALL/RET/RET_VOID/RET_STRUCT_SMALL

## Phase 13: Constant Folding at Lowering

**Expected gain: ~2-5%**

The lowering pass emits instructions for constant-to-constant operations that could be evaluated at compile time:

```
LOAD_INT  r0, 5
LOAD_INT  r1, 3
ADD_I     r2, r0, r1    ; could be: LOAD_INT r2, 8
```

Similarly, unary ops on immediates waste cycles:
```
LOAD_INT  r0, 5
NEG_I     r1, r0        ; could be: LOAD_INT r1, -5
```

### Implementation

This is best done in the SSA IR phase (before lowering) where constant values are tracked. A simple constant folding pass over the IR:

1. For each binary/unary operation where all operands are `Const` values, replace the result with a new `Const`.
2. Propagate through chains: if `Const(5) + Const(3)` → `Const(8)`, and `Const(8) * Const(2)` → `Const(16)`.
3. Handle integer, float, and boolean operations.

### Files

- `src/roxy/compiler/ir_builder.cpp` or a new `ir_constfold.cpp` — constant folding pass
- Alternatively, `src/roxy/compiler/lowering.cpp` — fold during emission when both operands are LOAD_INT/LOAD_CONST

## Phase 14: STRUCT_COPY with memcpy

**Expected gain: ~1-2%**

`STRUCT_COPY` currently uses a for-loop:

```cpp
for (u8 i = 0; i < slot_count; i++) {
    dst[i] = src[i];
}
```

The compiler may or may not optimize this to `memcpy`. Explicitly calling `memcpy` guarantees platform-specific SIMD paths for larger structs:

```cpp
memcpy(dst, src, slot_count * sizeof(u32));
```

### Files

- `src/roxy/vm/interpreter.cpp` — STRUCT_COPY handler

## Updated Summary

| Phase | Optimization | Expected Gain | Effort | Status |
|-------|-------------|---------------|--------|--------|
| 1 | Computed goto | 25-40% | Medium | Done |
| 2 | CALL/RET fast path | 10-20% | Low-Medium | Done |
| 3 | Fused compare+branch (integer) | 5-15% | Medium | Done |
| 4 | List index fast path | 5-10% | Low | Done |
| 5 | Minor optimizations | 1-5% each | Trivial-Low | Partial (5A done, 5B rejected, 5C not yet) |
| 6 | Immediate-operand arithmetic (ADDI) | 5-15% | Medium | Not started |
| 7 | Inline trivial natives (LIST_LEN, etc.) | 5-10% | Low-Medium | Not started |
| 8 | String constant interning | 5-20% | Low | Not started |
| 9 | Float compare-and-branch fusion | 3-8% | Low | Not started |
| 10 | Tail call optimization | 10-25% | High | Not started |
| 11 | Branch prediction hints | 2-5% | Trivial | Not started |
| 12 | Local stack base caching | 1-3% | Trivial | Not started |
| 13 | Constant folding at lowering | 2-5% | Medium | Not started |
| 14 | STRUCT_COPY with memcpy | 1-2% | Trivial | Not started |
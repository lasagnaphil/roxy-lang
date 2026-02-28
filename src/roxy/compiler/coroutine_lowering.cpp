#include "roxy/compiler/coroutine_lowering.hpp"
#include "roxy/compiler/types.hpp"
#include "roxy/core/format.hpp"

#include <cassert>
#include <cstring>

namespace rx {

// Helper to allocate a span from a vector using the bump allocator
template<typename T>
static Span<T> alloc_span(BumpAllocator& allocator, const Vector<T>& vec) {
    if (vec.empty()) return {};
    T* data = reinterpret_cast<T*>(allocator.alloc_bytes(sizeof(T) * vec.size(), alignof(T)));
    for (u32 i = 0; i < vec.size(); i++) {
        data[i] = vec[i];
    }
    return Span<T>(data, static_cast<u32>(vec.size()));
}

template<typename T>
static Span<T> alloc_span(BumpAllocator& allocator, u32 count) {
    if (count == 0) return {};
    T* data = reinterpret_cast<T*>(allocator.alloc_bytes(sizeof(T) * count, alignof(T)));
    return Span<T>(data, count);
}

// Allocate a persistent string in the bump allocator
static StringView alloc_string(BumpAllocator& allocator, const char* str) {
    u32 len = 0;
    while (str[len]) len++;
    char* buf = reinterpret_cast<char*>(allocator.alloc_bytes(len, 1));
    memcpy(buf, str, len);
    return StringView(buf, len);
}

// Allocate a persistent string from a StaticString
static StringView alloc_string_fmt(BumpAllocator& allocator, const char* fmt, StringView arg) {
    char tmp[256];
    format_to(tmp, sizeof(tmp), fmt, arg);
    return alloc_string(allocator, tmp);
}

// Sentinel state value for "coroutine is done".
// Must be a positive i32 value because struct fields are stored as 32-bit and
// zero-extended to 64-bit on load. A negative value like -1 would lose its sign
// extension when round-tripping through GET_FIELD/SET_FIELD.
static constexpr i32 CORO_STATE_DONE = 0x7FFFFFFF;

// Info about a yield point in the original coroutine
struct YieldPoint {
    u32 block_index;          // Index of the block containing the Yield instruction
    u32 inst_index;           // Index of the Yield instruction within the block
    ValueId yielded_value;    // The value being yielded
    BlockId resume_block_id;  // The resume block (target of the Goto after Yield)
};

// Info about a promoted variable (lives across yield points)
struct PromotedVar {
    StringView name;
    Type* type;
    u32 field_slot_offset;  // Slot offset in the coroutine struct
    u32 field_slot_count;   // Slot count of this field
};

// Get the number of u32 slots a type occupies
static u32 get_type_slot_count(Type* type) {
    if (!type) return 0;
    switch (type->kind) {
        case TypeKind::Bool:
        case TypeKind::I8: case TypeKind::U8:
        case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32:
        case TypeKind::F32:
        case TypeKind::Enum:
        case TypeKind::IntLiteral:
            return 1;
        case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F64:
        case TypeKind::Uniq:
        case TypeKind::Ref:
        case TypeKind::Weak:
        case TypeKind::List:
        case TypeKind::Map:
        case TypeKind::Coroutine:
            return 2;
        case TypeKind::Struct:
            return type->struct_info.slot_count;
        case TypeKind::String:
            return 2;  // Pointer
        default:
            return 0;
    }
}

// Create an IRInst in the bump allocator and emit it to a block
static IRInst* make_inst(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                          IROp op, Type* type) {
    IRInst* inst = allocator.emplace<IRInst>();
    inst->op = op;
    inst->type = type;
    inst->result = func->new_value();
    block->instructions.push_back(inst);
    return inst;
}

static ValueId emit_const_int(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               i64 value, Type* type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::ConstInt, type);
    inst->const_data.int_val = value;
    return inst->result;
}

static ValueId emit_const_bool(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                                bool value, Type* type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::ConstBool, type);
    inst->const_data.bool_val = value;
    return inst->result;
}

static ValueId emit_new(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                         StringView type_name, Type* result_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::New, result_type);
    inst->new_data.type_name = type_name;
    inst->new_data.args = Span<ValueId>();
    return inst->result;
}

static ValueId emit_get_field(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               ValueId object, StringView field_name,
                               u32 slot_offset, u32 slot_count, Type* result_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::GetField, result_type);
    inst->field.object = object;
    inst->field.field_name = field_name;
    inst->field.slot_offset = slot_offset;
    inst->field.slot_count = slot_count;
    return inst->result;
}

static ValueId emit_set_field(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                               ValueId object, StringView field_name,
                               u32 slot_offset, u32 slot_count, ValueId value, Type* type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::SetField, type);
    inst->field.object = object;
    inst->field.field_name = field_name;
    inst->field.slot_offset = slot_offset;
    inst->field.slot_count = slot_count;
    inst->store_value = value;
    return inst->result;
}

static ValueId emit_eq_i(BumpAllocator& allocator, IRFunction* func, IRBlock* block,
                           ValueId left, ValueId right, Type* bool_type) {
    IRInst* inst = make_inst(allocator, func, block, IROp::EqI, bool_type);
    inst->binary.left = left;
    inst->binary.right = right;
    return inst->result;
}

static void finish_goto(BumpAllocator& allocator, IRBlock* block, BlockId target,
                         Span<BlockArgPair> args = {}) {
    block->terminator.kind = TerminatorKind::Goto;
    block->terminator.goto_target.block = target;
    block->terminator.goto_target.args = args;
}

static void finish_branch(IRBlock* block, ValueId cond,
                           BlockId then_block, BlockId else_block) {
    block->terminator.kind = TerminatorKind::Branch;
    block->terminator.branch.condition = cond;
    block->terminator.branch.then_target.block = then_block;
    block->terminator.branch.then_target.args = {};
    block->terminator.branch.else_target.block = else_block;
    block->terminator.branch.else_target.args = {};
}

static void finish_return(IRBlock* block, ValueId value) {
    block->terminator.kind = TerminatorKind::Return;
    block->terminator.return_value = value;
}

static void finish_unreachable(IRBlock* block) {
    block->terminator.kind = TerminatorKind::Unreachable;
}

static IRBlock* create_block(BumpAllocator& allocator, IRFunction* func, StringView name) {
    IRBlock* block = allocator.emplace<IRBlock>();
    block->id = BlockId{static_cast<u32>(func->blocks.size())};
    block->name = name;
    func->blocks.push_back(block);
    return block;
}

// ===== Main lowering logic =====

static void lower_coroutine(IRFunction* original, IRModule* module,
                              BumpAllocator& allocator, TypeEnv& type_env) {
    TypeCache& types = type_env.types();
    Type* coro_yield_type = original->coro_yield_type;
    Type* coro_type = original->coro_type;

    // Step 1: Find all yield points in the original function
    Vector<YieldPoint> yield_points;
    for (u32 block_idx = 0; block_idx < original->blocks.size(); block_idx++) {
        IRBlock* block = original->blocks[block_idx];
        for (u32 inst_idx = 0; inst_idx < block->instructions.size(); inst_idx++) {
            IRInst* inst = block->instructions[inst_idx];
            if (inst->op == IROp::Yield) {
                YieldPoint yp;
                yp.block_index = block_idx;
                yp.inst_index = inst_idx;
                yp.yielded_value = inst->unary;

                // The resume block is the Goto target of this block's terminator
                assert(block->terminator.kind == TerminatorKind::Goto);
                yp.resume_block_id = block->terminator.goto_target.block;

                yield_points.push_back(yp);
            }
        }
    }

    // Step 2: Identify promoted variables from resume block parameters
    // These are local variables that need to be saved/restored across yield points
    Vector<PromotedVar> promoted_vars;
    // Use a map to deduplicate by name
    tsl::robin_map<StringView, u32> promoted_var_index;

    for (auto& yp : yield_points) {
        IRBlock* resume_block = original->blocks[yp.resume_block_id.id];
        for (auto& param : resume_block->params) {
            if (promoted_var_index.find(param.name) == promoted_var_index.end()) {
                PromotedVar pv;
                pv.name = param.name;
                pv.type = param.type;
                pv.field_slot_offset = 0;  // Computed below
                pv.field_slot_count = get_type_slot_count(param.type);
                promoted_var_index[param.name] = static_cast<u32>(promoted_vars.size());
                promoted_vars.push_back(pv);
            }
        }
    }

    // Build map from promoted var name to ALL original value IDs across all blocks.
    // A single promoted variable (e.g., loop var 'i') may appear as block params in
    // multiple blocks (e.g., for-header and resume block), each with different value IDs.
    tsl::robin_map<StringView, Vector<u32>> promoted_var_value_ids;
    for (auto* block : original->blocks) {
        for (auto& param : block->params) {
            if (promoted_var_index.find(param.name) != promoted_var_index.end()) {
                promoted_var_value_ids[param.name].push_back(param.value.id);
            }
        }
    }

    // Step 3: Build the coroutine struct type
    // Fields: __state (i32), __yield_val (T), params..., promoted locals...
    u32 num_params = static_cast<u32>(original->params.size());
    u32 num_fields = 2 + num_params + static_cast<u32>(promoted_vars.size());

    Vector<FieldInfo> fields;
    u32 current_slot = 0;

    // Field 0: __state (i32)
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__state");
        field.type = types.i32_type();
        field.is_pub = false;
        field.index = 0;
        field.slot_offset = current_slot;
        field.slot_count = 1;
        current_slot += 1;
        fields.push_back(field);
    }

    // Field 1: __yield_val (yield type)
    u32 yield_val_slot_offset = current_slot;
    u32 yield_val_slot_count = get_type_slot_count(coro_yield_type);
    {
        FieldInfo field;
        field.name = alloc_string(allocator, "__yield_val");
        field.type = coro_yield_type;
        field.is_pub = false;
        field.index = 1;
        field.slot_offset = current_slot;
        field.slot_count = yield_val_slot_count;
        current_slot += yield_val_slot_count;
        fields.push_back(field);
    }

    // Fields for each parameter
    for (u32 i = 0; i < num_params; i++) {
        FieldInfo field;
        field.name = original->params[i].name;
        field.type = original->params[i].type;
        field.is_pub = false;
        field.index = 2 + i;
        field.slot_offset = current_slot;
        field.slot_count = get_type_slot_count(original->params[i].type);
        current_slot += field.slot_count;
        fields.push_back(field);
    }

    // Fields for promoted locals
    for (u32 i = 0; i < promoted_vars.size(); i++) {
        promoted_vars[i].field_slot_offset = current_slot;
        FieldInfo field;
        field.name = promoted_vars[i].name;
        field.type = promoted_vars[i].type;
        field.is_pub = false;
        field.index = 2 + num_params + i;
        field.slot_offset = current_slot;
        field.slot_count = promoted_vars[i].field_slot_count;
        current_slot += field.slot_count;
        fields.push_back(field);
    }

    // Create the struct type
    StringView struct_name = alloc_string_fmt(allocator, "__coro_{}", original->name);

    // We create a new struct type for the coroutine state
    Type* struct_type = types.struct_type(struct_name, nullptr);
    struct_type->struct_info.fields = alloc_span(allocator, fields);
    struct_type->struct_info.slot_count = current_slot;
    struct_type->struct_info.constructors = Span<ConstructorInfo>();
    struct_type->struct_info.destructors = Span<DestructorInfo>();
    struct_type->struct_info.methods = Span<MethodInfo>();
    struct_type->struct_info.when_clauses = Span<WhenClauseInfo>();
    struct_type->struct_info.implemented_traits = Span<TraitImplRecord>();
    struct_type->struct_info.parent = nullptr;

    // Register it in the type env
    type_env.register_named_type(struct_name, struct_type);

    // Store the generated struct type on the coro type
    coro_type->coro_info.generated_struct_type = struct_type;

    Type* uniq_struct_type = types.uniq_type(struct_type);

    // Helper: find a field by name in the coroutine struct
    auto find_field = [&](StringView name) -> const FieldInfo* {
        for (auto& field : struct_type->struct_info.fields) {
            if (field.name == name) return &field;
        }
        return nullptr;
    };

    // ===== Generate init function =====
    // This replaces the original function. Same name, same params, returns Coro<T> (backed by uniq struct).
    IRFunction* init_func = allocator.emplace<IRFunction>();
    init_func->name = original->name;
    init_func->return_type = coro_type;

    // Copy parameters from original
    for (auto& param : original->params) {
        BlockParam new_param;
        new_param.value = init_func->new_value();
        new_param.type = param.type;
        new_param.name = param.name;
        init_func->params.push_back(new_param);
        init_func->param_is_ptr.push_back(false);
    }

    // Create entry block
    IRBlock* init_entry = create_block(allocator, init_func, alloc_string(allocator, "entry"));

    // Allocate the coroutine object
    ValueId obj = emit_new(allocator, init_func, init_entry, struct_name, uniq_struct_type);

    // Set __state = 0
    const FieldInfo* state_field = find_field(alloc_string(allocator, "__state"));
    ValueId zero = emit_const_int(allocator, init_func, init_entry, 0, types.i32_type());
    emit_set_field(allocator, init_func, init_entry, obj,
                   state_field->name, state_field->slot_offset, state_field->slot_count,
                   zero, types.i32_type());

    // Copy each parameter to the struct
    for (u32 i = 0; i < num_params; i++) {
        const FieldInfo* param_field = find_field(original->params[i].name);
        emit_set_field(allocator, init_func, init_entry, obj,
                       param_field->name, param_field->slot_offset, param_field->slot_count,
                       init_func->params[i].value, param_field->type);
    }

    // Return the object
    finish_return(init_entry, obj);

    // ===== Generate resume function =====
    // Name: __coro_<name>$$resume
    // Param: self (ref to coro struct)
    // Returns: yield_type
    StringView resume_name = alloc_string_fmt(allocator, "__coro_{}$$resume", original->name);
    IRFunction* resume_func = allocator.emplace<IRFunction>();
    resume_func->name = resume_name;
    resume_func->return_type = coro_yield_type;

    // Self parameter (ref to struct)
    Type* ref_struct_type = types.ref_type(struct_type);
    BlockParam self_param;
    self_param.value = resume_func->new_value();
    self_param.type = ref_struct_type;
    self_param.name = alloc_string(allocator, "self");
    resume_func->params.push_back(self_param);
    resume_func->param_is_ptr.push_back(false);

    // Create dispatch block
    IRBlock* dispatch_block = create_block(allocator, resume_func, alloc_string(allocator, "dispatch"));

    // Load state
    ValueId self_val = self_param.value;
    ValueId state_val = emit_get_field(allocator, resume_func, dispatch_block,
                                       self_val, state_field->name,
                                       state_field->slot_offset, state_field->slot_count,
                                       types.i32_type());

    // Create state blocks: one for each state (state 0 = entry, state N = after Nth yield)
    u32 num_states = static_cast<u32>(yield_points.size()) + 1;  // +1 for initial state
    Vector<IRBlock*> state_blocks;
    for (u32 i = 0; i < num_states; i++) {
        char name_buf[64];
        format_to(name_buf, sizeof(name_buf), "state_{}", i);
        state_blocks.push_back(create_block(allocator, resume_func, alloc_string(allocator, name_buf)));
    }

    // Create trap block (resume called after completion)
    IRBlock* trap_block = create_block(allocator, resume_func, alloc_string(allocator, "trap"));
    finish_unreachable(trap_block);

    // Build dispatch chain: if state == 0 goto state_0, elif state == 1 goto state_1, ... else trap
    IRBlock* current_dispatch = dispatch_block;
    for (u32 i = 0; i < num_states; i++) {
        ValueId state_const = emit_const_int(allocator, resume_func, current_dispatch, i, types.i32_type());
        ValueId is_match = emit_eq_i(allocator, resume_func, current_dispatch,
                                      state_val, state_const, types.bool_type());

        if (i == num_states - 1) {
            // Last state: branch to it or trap
            finish_branch(current_dispatch, is_match, state_blocks[i]->id, trap_block->id);
        } else {
            // Create next dispatch block for the chain
            char name_buf[64];
            format_to(name_buf, sizeof(name_buf), "dispatch_{}", i + 1);
            IRBlock* next_dispatch = create_block(allocator, resume_func,
                                                   alloc_string(allocator, name_buf));
            finish_branch(current_dispatch, is_match, state_blocks[i]->id, next_dispatch->id);
            current_dispatch = next_dispatch;
        }
    }

    // ===== Generate code for each state =====
    // Each state clones the reachable blocks from its entry point,
    // preserving the original control flow graph structure.
    // Yield instructions are replaced with save-state-and-return.

    // Build yield point lookup: block_index → yield_point_index
    tsl::robin_map<u32, u32> block_to_yield_idx;
    for (u32 i = 0; i < yield_points.size(); i++) {
        block_to_yield_idx[yield_points[i].block_index] = i;
    }

    // Helper to clone and remap a single instruction
    auto clone_inst = [&](IRBlock* new_block, IRInst* orig_inst,
                          tsl::robin_map<u32, ValueId>& vm) {
        IRInst* new_inst = allocator.emplace<IRInst>();
        *new_inst = *orig_inst;
        new_inst->result = resume_func->new_value();
        new_block->instructions.push_back(new_inst);
        vm[orig_inst->result.id] = new_inst->result;

        auto remap = [&](ValueId vid) -> ValueId {
            auto it = vm.find(vid.id);
            return (it != vm.end()) ? it->second : vid;
        };

        switch (new_inst->op) {
            case IROp::ConstNull: case IROp::ConstBool: case IROp::ConstInt:
            case IROp::ConstF: case IROp::ConstD: case IROp::ConstString:
            case IROp::StackAlloc: case IROp::VarAddr: case IROp::BlockArg:
                break;
            case IROp::GetField: case IROp::GetFieldAddr:
                new_inst->field.object = remap(new_inst->field.object);
                break;
            case IROp::SetField:
                new_inst->field.object = remap(new_inst->field.object);
                new_inst->store_value = remap(new_inst->store_value);
                break;
            case IROp::New: {
                Span<ValueId> old_args = new_inst->new_data.args;
                if (old_args.size() > 0) {
                    Span<ValueId> new_args = alloc_span<ValueId>(allocator, old_args.size());
                    for (u32 a = 0; a < old_args.size(); a++) new_args[a] = remap(old_args[a]);
                    new_inst->new_data.args = new_args;
                }
                break;
            }
            case IROp::Call: case IROp::CallNative: {
                Span<ValueId> old_args = new_inst->call.args;
                Span<ValueId> new_args = alloc_span<ValueId>(allocator, old_args.size());
                for (u32 a = 0; a < old_args.size(); a++) new_args[a] = remap(old_args[a]);
                new_inst->call.args = new_args;
                break;
            }
            case IROp::CallExternal: {
                Span<ValueId> old_args = new_inst->call_external.args;
                Span<ValueId> new_args = alloc_span<ValueId>(allocator, old_args.size());
                for (u32 a = 0; a < old_args.size(); a++) new_args[a] = remap(old_args[a]);
                new_inst->call_external.args = new_args;
                break;
            }
            case IROp::StructCopy:
                new_inst->struct_copy.dest_ptr = remap(orig_inst->struct_copy.dest_ptr);
                new_inst->struct_copy.source_ptr = remap(orig_inst->struct_copy.source_ptr);
                break;
            case IROp::LoadPtr:
                new_inst->load_ptr.ptr = remap(orig_inst->load_ptr.ptr);
                break;
            case IROp::StorePtr:
                new_inst->store_ptr.ptr = remap(orig_inst->store_ptr.ptr);
                new_inst->store_ptr.value = remap(orig_inst->store_ptr.value);
                break;
            case IROp::Cast:
                new_inst->cast.source = remap(orig_inst->cast.source);
                break;
            default:
                // Unary/binary ops
                new_inst->unary = remap(new_inst->unary);
                if (new_inst->op >= IROp::AddI && new_inst->op <= IROp::Shr) {
                    new_inst->binary.left = remap(orig_inst->binary.left);
                    new_inst->binary.right = remap(orig_inst->binary.right);
                }
                break;
        }
    };

    // For each state, clone reachable blocks from the entry point
    for (u32 state_idx = 0; state_idx < num_states; state_idx++) {
        // Determine entry block for this state
        u32 entry_orig_idx;
        if (state_idx == 0) {
            entry_orig_idx = 0;  // original entry block
        } else {
            entry_orig_idx = yield_points[state_idx - 1].resume_block_id.id;
        }

        // BFS to find reachable blocks, stopping at yield blocks
        Vector<u32> reachable;
        Vector<bool> visited(original->blocks.size(), false);
        Vector<u32> worklist;
        worklist.push_back(entry_orig_idx);
        visited[entry_orig_idx] = true;

        u32 wl_head = 0;
        while (wl_head < worklist.size()) {
            u32 block_idx = worklist[wl_head++];
            reachable.push_back(block_idx);

            // If this block has a yield, don't follow successors
            // (yield is replaced with save-and-return)
            if (block_to_yield_idx.count(block_idx)) continue;

            // Follow successors based on the terminator
            IRBlock* block = original->blocks[block_idx];
            auto add_successor = [&](BlockId id) {
                if (id.is_valid() && !visited[id.id]) {
                    visited[id.id] = true;
                    worklist.push_back(id.id);
                }
            };
            switch (block->terminator.kind) {
                case TerminatorKind::Goto:
                    add_successor(block->terminator.goto_target.block);
                    break;
                case TerminatorKind::Branch:
                    add_successor(block->terminator.branch.then_target.block);
                    add_successor(block->terminator.branch.else_target.block);
                    break;
                default:
                    break;
            }
        }

        // Create cloned blocks for all reachable blocks
        tsl::robin_map<u32, BlockId> block_map;
        for (u32 orig_idx : reachable) {
            char name_buf[64];
            format_to(name_buf, sizeof(name_buf), "s{}_b{}", state_idx, orig_idx);
            IRBlock* new_block = create_block(allocator, resume_func,
                                               alloc_string(allocator, name_buf));
            block_map[orig_idx] = new_block->id;
        }

        // State dispatch block gotos the first cloned block
        finish_goto(allocator, state_blocks[state_idx], block_map[entry_orig_idx]);

        // Fresh value_map for this state
        tsl::robin_map<u32, ValueId> vm;

        // The entry cloned block gets loads for params and/or promoted locals
        IRBlock* entry_new_block = resume_func->blocks[block_map[entry_orig_idx].id];

        // Always load params from struct
        for (u32 i = 0; i < num_params; i++) {
            const FieldInfo* param_field = find_field(original->params[i].name);
            ValueId loaded = emit_get_field(allocator, resume_func, entry_new_block,
                                            self_val, param_field->name,
                                            param_field->slot_offset, param_field->slot_count,
                                            param_field->type);
            vm[original->params[i].value.id] = loaded;
        }

        // Load promoted locals from struct at the entry block.
        // Map ALL value IDs for each promoted var (across all blocks in the
        // original function) to the entry-loaded value. This provides an
        // initial mapping that's valid before any block param updates.
        tsl::robin_map<u32, ValueId> entry_promoted_loads; // pv_idx -> loaded value
        for (auto& yp : yield_points) {
            IRBlock* resume_block = original->blocks[yp.resume_block_id.id];
            for (auto& param : resume_block->params) {
                auto pv_it = promoted_var_index.find(param.name);
                if (pv_it != promoted_var_index.end()) {
                    if (entry_promoted_loads.find(pv_it->second) != entry_promoted_loads.end()) continue;
                    PromotedVar& pv = promoted_vars[pv_it->second];
                    ValueId loaded = emit_get_field(allocator, resume_func, entry_new_block,
                                                    self_val, pv.name,
                                                    pv.field_slot_offset, pv.field_slot_count,
                                                    pv.type);
                    entry_promoted_loads[pv_it->second] = loaded;
                }
            }
        }
        // Map all value IDs for each promoted var to the entry-loaded value
        for (auto& [pv_idx, loaded] : entry_promoted_loads) {
            PromotedVar& pv = promoted_vars[pv_idx];
            auto vid_it = promoted_var_value_ids.find(pv.name);
            if (vid_it != promoted_var_value_ids.end()) {
                for (u32 vid : vid_it->second) {
                    vm[vid] = loaded;
                }
            }
        }

        // Pass 1: Clone block params for all non-entry reachable blocks.
        // For promoted variable params, still create block params (predecessors
        // pass values via goto/branch args) but DON'T override vm — the
        // promoted var values will be managed via struct store/load in Pass 2.
        // Track promoted block params for set_field emission in Pass 2.
        tsl::robin_map<u32, Vector<std::pair<ValueId, u32>>> promoted_block_params;
        for (u32 orig_idx : reachable) {
            if (orig_idx == entry_orig_idx) continue;
            IRBlock* orig_block = original->blocks[orig_idx];
            IRBlock* new_block = resume_func->blocks[block_map[orig_idx].id];
            for (auto& param : orig_block->params) {
                BlockParam new_param;
                new_param.value = resume_func->new_value();
                new_param.type = param.type;
                new_param.name = param.name;
                new_block->params.push_back(new_param);

                auto pv_it = promoted_var_index.find(param.name);
                if (pv_it != promoted_var_index.end()) {
                    // Promoted: track for set_field at block entry, don't override vm
                    promoted_block_params[new_block->id.id].push_back(
                        {new_param.value, pv_it->second});
                } else {
                    vm[param.value.id] = new_param.value;
                }
            }
        }

        // Pass 2: Clone instructions and terminators for each reachable block
        for (u32 orig_idx : reachable) {
            IRBlock* orig_block = original->blocks[orig_idx];
            IRBlock* new_block = resume_func->blocks[block_map[orig_idx].id];

            // For non-entry blocks: manage promoted vars via struct store/load.
            // This ensures promoted variable values are always correct regardless
            // of which path through the control flow graph reached this block.
            if (orig_idx != entry_orig_idx) {
                // Step A: Store promoted var block param values to the struct.
                // When a predecessor passes a new value (e.g., loop increment),
                // the block param receives it and we persist it to the struct.
                auto pbp_it = promoted_block_params.find(new_block->id.id);
                if (pbp_it != promoted_block_params.end()) {
                    for (auto& [bp_val, pv_idx] : pbp_it->second) {
                        PromotedVar& pv = promoted_vars[pv_idx];
                        emit_set_field(allocator, resume_func, new_block, self_val,
                                       pv.name, pv.field_slot_offset, pv.field_slot_count,
                                       bp_val, pv.type);
                    }
                }

                // Step B: Load all promoted vars from struct and update vm.
                // This gives each block a fresh, correct value for every promoted
                // variable, avoiding stale references to block params from
                // non-dominating blocks.
                for (auto& [pv_name, pv_idx] : promoted_var_index) {
                    PromotedVar& pv = promoted_vars[pv_idx];
                    ValueId loaded = emit_get_field(allocator, resume_func, new_block,
                                                    self_val, pv.name,
                                                    pv.field_slot_offset, pv.field_slot_count,
                                                    pv.type);
                    auto vid_it = promoted_var_value_ids.find(pv_name);
                    if (vid_it != promoted_var_value_ids.end()) {
                        for (u32 vid : vid_it->second) {
                            vm[vid] = loaded;
                        }
                    }
                }
            }

            // Check for yield in this block
            auto yield_it = block_to_yield_idx.find(orig_idx);
            bool has_yield = (yield_it != block_to_yield_idx.end());
            u32 inst_limit = has_yield
                ? yield_points[yield_it->second].inst_index
                : static_cast<u32>(orig_block->instructions.size());

            // Clone instructions (up to but not including the yield)
            for (u32 inst_idx = 0; inst_idx < inst_limit; inst_idx++) {
                clone_inst(new_block, orig_block->instructions[inst_idx], vm);
            }

            // Handle termination
            auto remap = [&](ValueId vid) -> ValueId {
                auto it = vm.find(vid.id);
                return (it != vm.end()) ? it->second : vid;
            };

            if (has_yield) {
                // Emit save-and-return for yield
                u32 yield_idx = yield_it->second;
                YieldPoint& yp = yield_points[yield_idx];
                u32 next_state = yield_idx + 1;

                // Save yield value to struct
                ValueId yielded = remap(yp.yielded_value);
                const FieldInfo* yield_field = find_field(alloc_string(allocator, "__yield_val"));
                emit_set_field(allocator, resume_func, new_block, self_val,
                               yield_field->name, yield_field->slot_offset, yield_field->slot_count,
                               yielded, coro_yield_type);

                // Set next state
                ValueId next_state_val = emit_const_int(allocator, resume_func, new_block,
                                                         next_state, types.i32_type());
                emit_set_field(allocator, resume_func, new_block, self_val,
                               state_field->name, state_field->slot_offset, state_field->slot_count,
                               next_state_val, types.i32_type());

                // Save promoted locals for the resume block
                IRBlock* next_resume = original->blocks[yp.resume_block_id.id];
                Terminator& orig_term = orig_block->terminator;
                for (u32 p = 0; p < next_resume->params.size(); p++) {
                    StringView var_name = next_resume->params[p].name;
                    auto pv_it = promoted_var_index.find(var_name);
                    if (pv_it != promoted_var_index.end()) {
                        PromotedVar& pv = promoted_vars[pv_it->second];
                        ValueId orig_arg_val = orig_term.goto_target.args[p].value;
                        ValueId remapped_val = remap(orig_arg_val);
                        emit_set_field(allocator, resume_func, new_block, self_val,
                                       pv.name, pv.field_slot_offset, pv.field_slot_count,
                                       remapped_val, pv.type);
                    }
                }

                // Return yield value
                ValueId result = emit_get_field(allocator, resume_func, new_block,
                                                self_val, yield_field->name,
                                                yield_field->slot_offset, yield_field->slot_count,
                                                coro_yield_type);
                finish_return(new_block, result);
            } else {
                // Clone the terminator, remapping block IDs and values
                switch (orig_block->terminator.kind) {
                    case TerminatorKind::Goto: {
                        JumpTarget& target = orig_block->terminator.goto_target;
                        auto target_it = block_map.find(target.block.id);
                        assert(target_it != block_map.end());
                        Span<BlockArgPair> new_args = {};
                        if (target.args.size() > 0) {
                            new_args = alloc_span<BlockArgPair>(allocator, target.args.size());
                            for (u32 a = 0; a < target.args.size(); a++) {
                                new_args[a].value = remap(target.args[a].value);
                            }
                        }
                        finish_goto(allocator, new_block, target_it->second, new_args);
                        break;
                    }
                    case TerminatorKind::Branch: {
                        auto& branch = orig_block->terminator.branch;
                        auto then_it = block_map.find(branch.then_target.block.id);
                        auto else_it = block_map.find(branch.else_target.block.id);
                        assert(then_it != block_map.end());
                        assert(else_it != block_map.end());

                        ValueId cond = remap(branch.condition);
                        new_block->terminator.kind = TerminatorKind::Branch;
                        new_block->terminator.branch.condition = cond;

                        new_block->terminator.branch.then_target.block = then_it->second;
                        if (branch.then_target.args.size() > 0) {
                            auto args = alloc_span<BlockArgPair>(allocator, branch.then_target.args.size());
                            for (u32 a = 0; a < branch.then_target.args.size(); a++) {
                                args[a].value = remap(branch.then_target.args[a].value);
                            }
                            new_block->terminator.branch.then_target.args = args;
                        } else {
                            new_block->terminator.branch.then_target.args = {};
                        }

                        new_block->terminator.branch.else_target.block = else_it->second;
                        if (branch.else_target.args.size() > 0) {
                            auto args = alloc_span<BlockArgPair>(allocator, branch.else_target.args.size());
                            for (u32 a = 0; a < branch.else_target.args.size(); a++) {
                                args[a].value = remap(branch.else_target.args[a].value);
                            }
                            new_block->terminator.branch.else_target.args = args;
                        } else {
                            new_block->terminator.branch.else_target.args = {};
                        }
                        break;
                    }
                    case TerminatorKind::Return: {
                        // In a coroutine, Return means "end of coroutine"
                        // Set state = DONE and return default value
                        ValueId done_val = emit_const_int(allocator, resume_func, new_block,
                                                           CORO_STATE_DONE, types.i32_type());
                        emit_set_field(allocator, resume_func, new_block, self_val,
                                       state_field->name, state_field->slot_offset, state_field->slot_count,
                                       done_val, types.i32_type());
                        ValueId default_val = emit_const_int(allocator, resume_func, new_block,
                                                              0, coro_yield_type);
                        finish_return(new_block, default_val);
                        break;
                    }
                    case TerminatorKind::Unreachable:
                        finish_unreachable(new_block);
                        break;
                    default:
                        break;
                }
            }
        }
    }

    // Renumber blocks to be contiguous (BlockId.id == array index)
    for (u32 i = 0; i < resume_func->blocks.size(); i++) {
        resume_func->blocks[i]->id = BlockId{i};
    }

    // ===== Generate done function =====
    StringView done_name = alloc_string_fmt(allocator, "__coro_{}$$done", original->name);
    IRFunction* done_func = allocator.emplace<IRFunction>();
    done_func->name = done_name;
    done_func->return_type = types.bool_type();

    // Self parameter
    BlockParam done_self;
    done_self.value = done_func->new_value();
    done_self.type = ref_struct_type;
    done_self.name = alloc_string(allocator, "self");
    done_func->params.push_back(done_self);
    done_func->param_is_ptr.push_back(false);

    IRBlock* done_entry = create_block(allocator, done_func, alloc_string(allocator, "entry"));
    ValueId done_self_val = done_self.value;
    ValueId done_state_val = emit_get_field(allocator, done_func, done_entry,
                                            done_self_val, state_field->name,
                                            state_field->slot_offset, state_field->slot_count,
                                            types.i32_type());
    ValueId done_sentinel = emit_const_int(allocator, done_func, done_entry, CORO_STATE_DONE, types.i32_type());
    ValueId is_done = emit_eq_i(allocator, done_func, done_entry,
                                 done_state_val, done_sentinel, types.bool_type());
    finish_return(done_entry, is_done);

    // ===== Replace in module =====
    // Find the original function in the module and replace with init
    for (u32 i = 0; i < module->functions.size(); i++) {
        if (module->functions[i] == original) {
            module->functions[i] = init_func;
            break;
        }
    }
    // Add resume and done functions
    module->functions.push_back(resume_func);
    module->functions.push_back(done_func);
}

void coroutine_lower(IRModule* module, BumpAllocator& allocator, TypeEnv& type_env) {
    // Collect coroutine functions first (we'll modify the function list)
    Vector<IRFunction*> coroutine_funcs;
    for (auto* func : module->functions) {
        if (func->is_coroutine) {
            coroutine_funcs.push_back(func);
        }
    }

    // Lower each coroutine
    for (auto* func : coroutine_funcs) {
        lower_coroutine(func, module, allocator, type_env);
    }
}

}

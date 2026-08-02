// IRBuilder — ownership and cleanup bookkeeping: local-scope maps, owned-local
// tracking (consume / move-marking / string retain-release), implicit
// destruction, field cleanup, and exception-path cleanup records. The tracked
// state and its keyed lookups live in the OwnershipTracker collaborator
// (ownership_tracker.{hpp,cpp}); this TU keeps the IR-emitting transitions
// that consult it. Split out of ir_builder.cpp; file-internal helpers shared
// across the ir_builder*.cpp TUs live in ir_builder_internal.hpp.

#include "roxy/compiler/ir_builder.hpp"

#include "ir_builder_internal.hpp"

namespace rx {

using namespace ir_builder_detail;

void IRBuilder::define_local(StringView name, ValueId value, Type* type, bool is_ptr) {
    if (m_local_scopes.empty()) return;

    // Search for an existing binding in outer scopes and update it
    // This is necessary for SSA - assignments should update the existing definition
    // Sound for declarations too: semantic analysis rejects a local shadowing
    // another local/parameter of the same function (check_no_local_shadowing),
    // so a declaration can only find an existing binding for a name whose
    // previous scope has already been popped — never a live outer binding.
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            // Update value/type; keep the existing is_ptr — ptr-ness is fixed at
            // definition (an out/inout param's SSA updates store through the
            // pointer, they never rebind it to a non-pointer). (§3.7)
            LocalVar& lv = it.value();
            lv.value = value;
            lv.type = type;
            return;
        }
    }

    // If no existing binding, add to innermost scope (new variable declaration)
    m_local_scopes.back()[name] = {value, type, is_ptr};
}

ValueId IRBuilder::lookup_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return it->second.value;
        }
    }
    report_error(intern_format("Internal error: undefined variable '{}' in IR generation (fn {})",
                               name, m_current_func ? m_current_func->name : "?"_sv).data());
    return ValueId::invalid();
}

void IRBuilder::push_scope() {
    m_local_scopes.push_back({});
}

void IRBuilder::pop_scope() {
    if (m_local_scopes.empty()) return;
    u32 depth = static_cast<u32>(m_local_scopes.size());

    // Record cleanup info for exception-path cleanup BEFORE emit_scope_cleanup.
    record_scope_cleanup_records(depth);

    // Emit cleanup for live owned locals in this scope
    emit_scope_cleanup(depth);

    // Remove owned local tracking for this scope
    m_ownership.pop_to_depth(depth);

    m_local_scopes.pop_back();
}

BlockId IRBuilder::current_or_last_block_id() const {
    if (m_current_block) return m_current_block->id;
    if (!m_current_func->blocks.empty()) return m_current_func->blocks.back()->id;
    return BlockId::invalid();
}

void IRBuilder::record_scope_cleanup_records(u32 depth) {
    BlockId end_block = current_or_last_block_id();
    if (!end_block.is_valid()) return;
    for (u32 i = 0; i < m_ownership.count(); i++) {
        const OwnedLocalInfo& info = m_ownership.entry(i);
        if (info.scope_depth < depth) continue;
        if (!info.start_block.is_valid() || !info.initial_value.is_valid()) continue;
        IRCleanupKind kind = info.kind == OwnedKind::RefBorrow ? IRCleanupKind::RefDec
                           : info.kind == OwnedKind::StrOwn    ? IRCleanupKind::StrRelease
                           : IRCleanupKind::Delete;
        m_current_func->cleanup_info.push_back(
            {info.initial_value, info.type, info.start_block, end_block, kind});
    }
}

IRBuilder::LocalVar* IRBuilder::find_local(StringView name) {
    // Search from innermost to outermost scope
    for (i32 i = static_cast<i32>(m_local_scopes.size()) - 1; i >= 0; i--) {
        auto it = m_local_scopes[i].find(name);
        if (it != m_local_scopes[i].end()) {
            return &it.value();
        }
    }
    return nullptr;
}

void IRBuilder::track_noncopyable_call_temp(ValueId val, Type* type) {
    // Keyed on drop glue rather than move-only-ness (`tracked_for_cleanup`): a
    // temporary is tracked because someone has to destroy it, which has nothing
    // to do with whether binding it would move its source.
    if (!tracked_for_cleanup(type) || !m_current_block || !val.is_valid()) return;
    // Skip if already tracked as a temporary (constructor/struct-literal paths
    // self-track their heap temps at creation).
    if (m_ownership.has_temp_for(val)) return;
    StringView temp_name = intern_synthetic_name("__tmp", m_next_temp_id++);
    define_local(temp_name, val, type);
    u32 scope_depth = static_cast<u32>(m_local_scopes.size());
    m_ownership.track({temp_name, type, scope_depth, false, true,
                       m_current_block->id, val});
}

void IRBuilder::track_string_temp(ValueId val, Type* type) {
    if (!type || type->kind != TypeKind::String || !m_current_block || !val.is_valid()) return;
    // Skip if already tracked as a temporary (avoid double-tracking).
    if (m_ownership.has_temp_for(val)) return;
    StringView temp_name = intern_synthetic_name("__str", m_next_temp_id++);
    define_local(temp_name, val, type);
    u32 scope_depth = static_cast<u32>(m_local_scopes.size());
    m_ownership.track({temp_name, type, scope_depth, false, /*is_temporary=*/true,
                       m_current_block->id, val, OwnedKind::StrOwn});
}

void IRBuilder::track_ref_call_temp(ValueId val, Type* type) {
    if (!type || type->kind != TypeKind::Ref || !m_current_block || !val.is_valid()) return;
    // Skip if already tracked as a temporary (avoid double-tracking).
    if (m_ownership.has_temp_for(val)) return;
    StringView temp_name = intern_synthetic_name("__ref", m_next_temp_id++);
    define_local(temp_name, val, type);
    u32 scope_depth = static_cast<u32>(m_local_scopes.size());
    m_ownership.track({temp_name, type, scope_depth, false, /*is_temporary=*/true,
                       m_current_block->id, val, OwnedKind::RefBorrow});
    // The scope-exit RefDec names this ValueId; copy propagation folding it back
    // into the producing call would move the decrement onto the wrong value.
    // Same pin a `ref` local takes (see gen_var_decl).
    pin_tracked_value(val);
}

void IRBuilder::acquire_ref_borrow(ValueId val, Expr* source) {
    if (is_ref_handoff_source(source)) {
        // A ref-returning call already carries exactly one count for us to
        // adopt. End the temporary's tracking so the binding — not the temp —
        // is what releases it; otherwise both decrement and the count underflows.
        OwnedLocalInfo* temp = m_ownership.find_live_temp(val);
        if (temp && temp->kind == OwnedKind::RefBorrow) temp->is_moved = true;
        return;
    }
    // Any other source is a fresh borrow alongside a still-live owner.
    // `ref x = self` is a promotion: the inc is heap-gated.
    emit_ref_borrow_inc(val, source);
}

void IRBuilder::consume_or_retain_string(ValueId val, Type* type, bool adopted_by_variable) {
    if (!type || type->kind != TypeKind::String || !val.is_valid()) return;
    // A tracked owned string temp: adopt its count-1 ownership (consume the temp)
    // rather than retaining a second reference.
    OwnedLocalInfo* info = m_ownership.find_live_temp(val);
    if (info && info->kind == OwnedKind::StrOwn) {
        info->is_moved = true;  // adopt: ownership transfers to the destination
        if (!adopted_by_variable) {
            // Stored into a field/container/global (not sharing a tracked
            // local's register): end the temp's cleanup record and null its
            // mapping so its scope-exit release is suppressed.
            emit_nullify(val);
            ValueId null_val = emit_const_null();
            define_local(info->name, null_val, info->type);
        }
        return;
    }
    // Not a fresh temp — an existing owner (identifier, borrowed field/element
    // read) is being copied, so retain to create the destination's own count.
    emit_str_retain(val);
}

void IRBuilder::consume_temp_noncopyable(ValueId val, bool adopted_by_variable) {
    // Find the live temporary produced as `val` (temporaries have __tmp names).
    // Only matches temporaries, not named variables that happen to share the
    // same ValueId (the tracker's value index holds temporaries only).
    OwnedLocalInfo* info = m_ownership.find_live_temp(val);
    if (!info) return;  // not a tracked temporary (named variable / copyable type)
    info->is_moved = true;
    // When adopted by a variable (same register), the variable's cleanup record
    // handles destruction — no Nullify needed. Otherwise, emit a Nullify annotation
    // so the bytecode builder ends the cleanup record scope at this point.
    if (!adopted_by_variable) {
        emit_nullify(val);
    }
    // Update the local mapping to null so yield/block-arg captures see null
    // instead of the stale pointer (prevents double-free in coroutines).
    ValueId null_val = emit_const_null();
    define_local(info->name, null_val, info->type);
}

void IRBuilder::mark_moved_from(StringView name, bool null_ssa, bool nullify_record) {
    OwnedLocalInfo* owned_info = m_ownership.find_by_name(name);
    if (!owned_info || owned_info->is_moved) return;

    // For uniq sources, re-point the SSA name at null so future reads (and the
    // scope-exit Delete) see null instead of the moved-out pointer. Value-struct
    // sources keep their register — the bitwise copy already transferred them.
    if (null_ssa && owned_info->type && owned_info->type->kind == TypeKind::Uniq) {
        ValueId null_val = emit_const_null();
        define_local(name, null_val, owned_info->type);
    }

    // Zero the cleanup record's register so exception-path cleanup skips it.
    if (nullify_record && owned_info->initial_value.is_valid()) {
        emit_nullify(owned_info->initial_value);
    }

    owned_info->is_moved = true;
}

void IRBuilder::nullify_moved_field_source(Expr* consumed) {
    if (!consumed || consumed->kind != AstKind::ExprGet) return;
    Type* field_type = consumed->resolved_type;
    if (!field_type || field_type->is_copy()) return;

    GetExpr& src_get = consumed->get;
    Type* src_obj_type = src_get.object->resolved_type;
    Type* src_struct_type = src_obj_type ? src_obj_type->base_type() : nullptr;
    if (!src_struct_type || !src_struct_type->is_struct()) return;
    const FieldInfo* src_field = src_struct_type->struct_info.find_field(src_get.name);
    if (!src_field) return;

    ValueId src_obj_ptr = gen_expr(src_get.object);
    ValueId null_val = emit_const_null();
    // Tag the store with the field's real type (not void): the C backend keys
    // off the SetField type to cast the null (`void*`) to a `uniq`/`ref` pointer
    // field — `field = nullptr` is ill-formed otherwise in C++.
    emit_set_field(src_obj_ptr, src_field->name, src_field->slot_offset,
                   src_field->slot_count, null_val, field_type);
}

void IRBuilder::emit_implicit_destroy(OwnedLocalInfo& info) {
    if (info.is_moved) return;
    if (!m_current_block) return;  // Block already terminated

    ValueId current_value = lookup_local(info.name);

    // Ref borrow: decrement its count rather than destroy the pointee. The
    // owner is freed elsewhere; this just releases this binding's borrow.
    if (info.kind == OwnedKind::RefBorrow) {
        emit_ref_dec(current_value);
        // Narrow the exception cleanup record to end at this RefDec so the
        // unwind path doesn't double-decrement after the normal-path RefDec
        // (mirrors the owned-local Nullify below).
        if (info.initial_value.is_valid()) {
            emit_nullify(info.initial_value);
        }
        info.is_moved = true;
        return;
    }

    // Owned string local: release (owner--; free at zero). Like a ref borrow it
    // releases a count rather than unconditionally freeing; the Nullify narrows
    // the exception cleanup record so unwind doesn't double-release (finding 9b).
    if (info.kind == OwnedKind::StrOwn) {
        emit_str_release(current_value);
        if (info.initial_value.is_valid()) {
            emit_nullify(info.initial_value);
        }
        info.is_moved = true;
        return;
    }

    // A caught exception bound to a catch-all (`ExceptionRef`) has no compile-time
    // concrete type, so free it type-erased via a raw object free (void-typed
    // Delete → DEL_OBJ). This reclaims the memory (finding 9a); the caught type's
    // `fun delete` does not run — a type-erased free can't reach a bytecode
    // destructor, the same limitation as the unhandled-exception path.
    if (info.type && info.type->kind == TypeKind::ExceptionRef) {
        emit_delete(current_value, m_types.void_type());
        if (info.initial_value.is_valid()) {
            emit_nullify(info.initial_value);
        }
        info.is_moved = true;
        return;
    }

    // Emit a single typed Delete — the runtime handles null checks,
    // destructor calls, container element iteration, and freeing.
    emit_delete(current_value, info.type);

    // Null-ify heap-allocated values to prevent double-cleanup from exception handler.
    // Use a Nullify annotation (not a runtime ConstNull) so the bytecode builder
    // narrows the cleanup record scope instead of zeroing the register.
    //
    // Pointer-shaped only, and deliberately so: the C backend lowers Nullify on a
    // value struct to a `memset` of its backing storage, which is a real runtime
    // action rather than a record narrowing. Widening this to every owned local
    // therefore zeroed structs that were still being read (`return o;` after a
    // move, most visibly).
    if (holds_owning_pointer(info.type)) {
        if (info.initial_value.is_valid()) {
            emit_nullify(info.initial_value);
        }
    }

    info.is_moved = true;  // Prevent double-destroy
}

void IRBuilder::emit_single_field_destroy(ValueId obj_ptr, StringView field_name,
                                          u32 slot_offset, u32 slot_count, Type* field_type) {
    // For struct fields stored as addresses (value-type structs), use GetFieldAddr.
    // The caller has already decided this field needs dropping; this only picks
    // the addressing mode, so it asks about representation, not move-only-ness.
    if (field_type->is_struct()) {
        ValueId field_addr = emit_get_field_addr(obj_ptr, field_name,
            slot_offset, field_type);
        emit_delete(field_addr, field_type);
        return;
    }

    // For pointer-valued fields (uniq, list, map): load the pointer and emit typed Delete
    ValueId field_val = emit_get_field(obj_ptr, field_name,
        slot_offset, slot_count, field_type);
    emit_delete(field_val, field_type);
}

void IRBuilder::emit_field_cleanup(ValueId self_ptr, Type* struct_type) {
    StructTypeInfo& struct_info = struct_type->struct_info;

    // Clean only this struct's OWN fields. `struct_info.fields` is
    // parent-prefixed (inherited fields first, then own — see semantic.cpp's
    // member resolution and inheritance.md), and the destructor chains to the
    // parent, whose own destructor cleans the inherited ones. Walking the whole
    // span destroyed an inherited `uniq` twice: once here, once in the parent
    // (caught by the interpreter's double-delete assert; a release build would
    // have double-freed). A parent with owned fields always has a destructor,
    // so nothing is left uncleaned by stopping at the boundary.
    //
    // `when_clauses` need no such split: they are resolved from the struct's
    // own declarations and are not inherited.
    Type* parent_type = struct_info.parent;
    u32 own_field_start = (parent_type && parent_type->is_struct())
                              ? static_cast<u32>(parent_type->struct_info.fields.size())
                              : 0;

    // Process regular fields in reverse order (LIFO, like C++ member destruction)
    for (i32 i = static_cast<i32>(struct_info.fields.size()) - 1;
         i >= static_cast<i32>(own_field_start); i--) {
        const FieldInfo& field = struct_info.fields[i];
        if (!field.type) continue;

        // One gate, shared with the synthetic-destructor pass and both backends'
        // container drops (member_needs_drop → compute_drop_plan). Restating it
        // here as `Uniq || noncopyable()` is what silently skipped `string`
        // fields; a typed Delete on a string field lowers to StrRelease on both
        // backends, so routing them through the same path is all it takes.
        if (!member_needs_drop(field.type)) continue;

        if (field.type->kind == TypeKind::Ref) {
            // A `ref` field is a counted borrow — release it. Only synthesized
            // closure envs hold ref fields ([ref self] / captured ref locals);
            // ref is banned from user struct fields, so this is inert elsewhere.
            ValueId ref_val = emit_get_field(self_ptr, field.name,
                field.slot_offset, field.slot_count, field.type);
            emit_ref_dec(ref_val);
        } else {
            emit_single_field_destroy(self_ptr, field.name,
                field.slot_offset, field.slot_count, field.type);
        }
    }

    // Process variant fields in when clauses (discriminant-aware cleanup).
    // Same `member_needs_drop` gate as the regular fields above and as
    // build_struct_field_deletes — this arm used to restate it as
    // `Uniq || noncopyable()`, which omits `string`, so a tagged union with a
    // `string` variant field synthesized an EMPTY destructor. The VM hid that
    // whenever the struct was walk-eligible (the descriptor uses the shared
    // gate), but an inherited struct — and every struct on the C backend, which
    // always calls `$$delete` — leaked the variant's string.
    for (const auto& clause : struct_info.when_clauses) {
        bool has_droppable_variant = false;
        for (const auto& variant : clause.variants) {
            for (const auto& variant_field : variant.fields) {
                if (member_needs_drop(variant_field.type)) {
                    has_droppable_variant = true;
                    break;
                }
            }
            if (has_droppable_variant) break;
        }
        if (!has_droppable_variant) continue;

        // Load the discriminant value
        const FieldInfo* disc_field = nullptr;
        for (const auto& field : struct_info.fields) {
            if (field.name == clause.discriminant_name) {
                disc_field = &field;
                break;
            }
        }
        if (!disc_field) continue;

        ValueId disc_val = emit_get_field(self_ptr, disc_field->name,
            disc_field->slot_offset, disc_field->slot_count, disc_field->type);

        // Create a merge block for after all variant cleanup
        IRBlock* merge_block = create_block("variant_cleanup_done");

        // For each variant, emit: if disc == variant_value, clean up that variant's fields
        for (u32 vi = 0; vi < clause.variants.size(); vi++) {
            const auto& variant = clause.variants[vi];

            bool variant_has_cleanup = false;
            for (const auto& variant_field : variant.fields) {
                if (member_needs_drop(variant_field.type)) {
                    variant_has_cleanup = true;
                    break;
                }
            }
            if (!variant_has_cleanup) continue;

            // Compare discriminant to this variant's value
            ValueId variant_val = emit_const_int(
                static_cast<i32>(variant.discriminant_value), disc_field->type);
            ValueId is_match = emit_binary(IROp::EqI, disc_val, variant_val, m_types.bool_type());

            IRBlock* cleanup_block = create_block("variant_cleanup");
            IRBlock* next_block = create_block("variant_next");

            finish_block_branch(is_match, cleanup_block->id, next_block->id);

            set_current_block(cleanup_block);
            for (i32 fi = static_cast<i32>(variant.fields.size()) - 1; fi >= 0; fi--) {
                const auto& variant_field = variant.fields[fi];
                if (!member_needs_drop(variant_field.type)) continue;
                u32 actual_offset = clause.union_slot_offset + variant_field.slot_offset;
                emit_single_field_destroy(self_ptr, variant_field.name,
                    actual_offset, variant_field.slot_count, variant_field.type);
            }
            finish_block_goto(merge_block->id);

            set_current_block(next_block);
        }

        // Fall through to merge block from last next_block
        finish_block_goto(merge_block->id);
        set_current_block(merge_block);
    }
}

void IRBuilder::emit_struct_clone_glue(ValueId struct_ptr, Type* struct_type) {
    if (!struct_type || !struct_type->is_struct() || !struct_ptr.is_valid()) return;
    if (!m_current_block) return;  // block already terminated

    // A move-only struct is never duplicated — its copies are moves, and the
    // source hands its counts over rather than sharing them. Nothing to acquire.
    // (This is also what keeps the walk below total: only a copyable struct
    // reaches it, and a copyable struct cannot hold a member whose drop has no
    // inverse — that member is exactly what would have made it move-only.)
    if (struct_type->noncopyable()) return;

    StructTypeInfo& struct_info = struct_type->struct_info;

    // Every field, inherited ones included. This is the one place the walk must
    // NOT mirror emit_field_cleanup's parent-boundary split: cleanup stops at the
    // boundary because the destructor chains to the parent's, whereas a copy is a
    // flat memcpy of the whole layout with nothing to chain to. Stopping early
    // would duplicate an inherited member without retaining it.
    for (const auto& field : struct_info.fields) {
        if (!field.type) continue;
        // The same gate as the drop (member_needs_drop → compute_drop_plan), so
        // retain and release are inverses by construction rather than by review:
        // a member acquires a count here exactly when it releases one on
        // teardown, and the two turn on together.
        if (!member_needs_drop(field.type)) continue;
        emit_member_retain(struct_ptr, field.name, field.slot_offset,
                           field.slot_count, field.type);
    }

    // Variant fields live in the union, so only the members named by the current
    // discriminant are actually there to retain — same discriminant-guarded shape
    // as emit_field_cleanup and build_struct_field_deletes.
    for (const auto& clause : struct_info.when_clauses) {
        bool any_retaining_variant = false;
        for (const auto& variant : clause.variants) {
            for (const auto& variant_field : variant.fields) {
                if (variant_field.type && member_needs_drop(variant_field.type)) {
                    any_retaining_variant = true;
                    break;
                }
            }
            if (any_retaining_variant) break;
        }
        if (!any_retaining_variant) continue;

        const FieldInfo* disc_field = struct_info.find_field(clause.discriminant_name);
        if (!disc_field) continue;

        ValueId disc_val = emit_get_field(struct_ptr, disc_field->name,
            disc_field->slot_offset, disc_field->slot_count, disc_field->type);

        IRBlock* merge_block = create_block("variant_retain_done");

        for (const auto& variant : clause.variants) {
            bool variant_retains = false;
            for (const auto& variant_field : variant.fields) {
                if (variant_field.type && member_needs_drop(variant_field.type)) {
                    variant_retains = true;
                    break;
                }
            }
            if (!variant_retains) continue;

            ValueId variant_val = emit_const_int(
                static_cast<i32>(variant.discriminant_value), disc_field->type);
            ValueId is_match = emit_binary(IROp::EqI, disc_val, variant_val, m_types.bool_type());

            IRBlock* retain_block = create_block("variant_retain");
            IRBlock* next_block = create_block("variant_retain_next");
            finish_block_branch(is_match, retain_block->id, next_block->id);

            set_current_block(retain_block);
            for (const auto& variant_field : variant.fields) {
                if (!variant_field.type) continue;
                if (!member_needs_drop(variant_field.type)) continue;
                emit_member_retain(struct_ptr, variant_field.name,
                                   clause.union_slot_offset + variant_field.slot_offset,
                                   variant_field.slot_count, variant_field.type);
            }
            finish_block_goto(merge_block->id);

            set_current_block(next_block);
        }

        finish_block_goto(merge_block->id);
        set_current_block(merge_block);
    }
}

void IRBuilder::emit_value_retain(ValueId value, Type* type) {
    emit_value_retain(value, type, compute_retain_plan(type));
}

void IRBuilder::emit_value_retain(ValueId value, Type* type, const RetainPlan& plan) {
    switch (plan.kind) {
    case RetainKind::StrRetain:
        emit_str_retain(value);
        break;
    case RetainKind::RefInc:
        emit_ref_inc(value);
        break;
    case RetainKind::WalkFields:
        // A value struct is represented by its address (the IR-wide convention),
        // so `value` is already what the recursive walk needs. Terminating: a
        // direct value cycle is rejected as infinitely sized (recursive-types.md),
        // so the field graph is a DAG.
        emit_struct_clone_glue(value, plan.struct_type);
        break;
    case RetainKind::None:
        // A value that drops but cannot be retained — `uniq`, a container, a
        // coroutine. Holding one is precisely what makes a type move-only, so
        // reaching here means the move-only derivation and the drop derivation
        // have gone out of sync (lifetimes.md: "a type is move-only exactly when
        // its drop has no inverse"). Duplicating it anyway would hand two owners
        // the same pointer.
        report_error("Internal error: duplicating a value that cannot be retained");
        break;
    }
}

void IRBuilder::emit_member_retain(ValueId struct_ptr, StringView field_name,
                                   u32 slot_offset, u32 slot_count, Type* field_type) {
    // A nested value struct lives inline in the enclosing layout, so its "value"
    // is its address; everything else is loaded out of the slot. The plan is
    // computed once and passed down — `compute_retain_plan` recurses through a
    // struct's fields, so recomputing it inside emit_value_retain would walk the
    // whole field subtree a second time, per field, at every clone site.
    RetainPlan plan = compute_retain_plan(field_type);
    ValueId member = plan.kind == RetainKind::WalkFields
        ? emit_get_field_addr(struct_ptr, field_name, slot_offset, field_type)
        : emit_get_field(struct_ptr, field_name, slot_offset, slot_count, field_type);
    emit_value_retain(member, field_type, plan);
}

void IRBuilder::emit_discriminant_reassign_cleanup(ValueId obj,
                                                   const WhenClauseInfo& clause,
                                                   ValueId new_disc) {
    // Reassigning `s.kind` moves the tagged union to a different variant. Two
    // hazards, both because the destructor's variant cleanup is guarded by the
    // *current* discriminant (lowering.cpp build_struct_field_deletes /
    // emit_field_cleanup): (1) the outgoing variant's owned fields would never be
    // freed once the discriminant no longer names them → leak; (2) the incoming
    // variant would read the outgoing variant's leftover union bytes as its own
    // owned field, so teardown frees a garbage pointer → crash / double-free.
    // Fix: drop the outgoing variant's owned fields, then zero the union so the
    // incoming variant starts from null. A no-op when the variant isn't changing.
    // Shared gate again: re-tagging away from a variant must release exactly
    // what teardown would have, `string` variant fields included.
    auto variant_has_owned = [](const VariantInfo& v) {
        for (const auto& f : v.fields)
            if (member_needs_drop(f.type)) return true;
        return false;
    };

    bool any_owned = false;
    for (const auto& v : clause.variants)
        if (variant_has_owned(v)) { any_owned = true; break; }
    if (!any_owned) return;   // trivial variants: re-tagging is already safe

    ValueId disc_old = emit_get_field(obj, clause.discriminant_name,
        clause.discriminant_slot_offset, 1, clause.discriminant_type);

    // Skip everything when re-tagging to the same variant (a no-op that must
    // preserve the current owned field rather than free it).
    ValueId changed = emit_binary(IROp::NeI, disc_old, new_disc, m_types.bool_type());
    IRBlock* cleanup_block = create_block("disc_reassign_cleanup");
    IRBlock* done_block = create_block("disc_reassign_done");
    finish_block_branch(changed, cleanup_block->id, done_block->id);
    set_current_block(cleanup_block);

    // Drop the currently-active variant's owned fields (guarded by the old
    // discriminant), mirroring emit_field_cleanup's per-variant dispatch.
    IRBlock* drop_merge = create_block("disc_reassign_drop_done");
    for (const auto& variant : clause.variants) {
        if (!variant_has_owned(variant)) continue;
        ValueId variant_val = emit_const_int(
            static_cast<i32>(variant.discriminant_value), clause.discriminant_type);
        ValueId is_match = emit_binary(IROp::EqI, disc_old, variant_val, m_types.bool_type());
        IRBlock* drop_block = create_block("disc_reassign_drop");
        IRBlock* next_block = create_block("disc_reassign_next");
        finish_block_branch(is_match, drop_block->id, next_block->id);

        set_current_block(drop_block);
        for (i32 fi = static_cast<i32>(variant.fields.size()) - 1; fi >= 0; fi--) {
            const auto& variant_field = variant.fields[fi];
            if (!member_needs_drop(variant_field.type)) continue;
            emit_single_field_destroy(obj, variant_field.name,
                clause.union_slot_offset + variant_field.slot_offset,
                variant_field.slot_count, variant_field.type);
        }
        finish_block_goto(drop_merge->id);
        set_current_block(next_block);
    }
    finish_block_goto(drop_merge->id);
    set_current_block(drop_merge);

    // Clear the whole union: the just-freed slots lose their dangling pointers,
    // and any other variant's leftover bytes are zeroed, so the incoming
    // variant's owned fields read null (a safe no-op at teardown until set).
    emit_zero_slots(obj, clause.union_slot_offset, clause.union_slot_count);
    finish_block_goto(done_block->id);
    set_current_block(done_block);
}

// emit_element_destroy, emit_list_cleanup, and emit_map_cleanup have been
// removed — all container/element cleanup is now handled by the typed
// IROp::Delete instruction which lowers to the DELETE bytecode opcode.

void IRBuilder::emit_scope_cleanup(u32 min_scope_depth) {
    if (!m_current_block) return;  // Block already terminated

    // LIFO order (reverse declaration order, like C++ destructors)
    for (i32 i = static_cast<i32>(m_ownership.count()) - 1; i >= 0; i--) {
        OwnedLocalInfo& info = m_ownership.entry(static_cast<u32>(i));
        if (info.scope_depth >= min_scope_depth && !info.is_moved) {
            emit_implicit_destroy(info);
        }
    }
}

}

#pragma once

#include "roxy/core/types.hpp"
#include "roxy/core/string_view.hpp"
#include "roxy/core/vector.hpp"
#include "roxy/compiler/ssa_ir.hpp"

#include "roxy/core/tsl/robin_map.h"

namespace rx {

class Type;

// Whether a tracked local owns its value (destroy on cleanup), is a `ref`
// borrow (decrement its count on cleanup), or an owned string (release on
// cleanup). RefBorrow/StrOwn locals reuse the owned-local machinery (LIFO
// scope cleanup, exception records, liveness) with a different cleanup op.
enum class OwnedKind : u8 { Owned, RefBorrow, StrOwn };

// A local (or compiler temporary) that owns a value needing cleanup: uniq
// references, value structs with destructors, containers, ref borrows, and
// owned strings.
struct OwnedLocalInfo {
    StringView name;
    Type* type;            // Full variable type (uniq T or struct T)
    u32 scope_depth;       // Scope level where declared
    bool is_moved;         // Ownership transferred (pass, return, explicit delete)
    bool is_temporary;     // True for compiler-generated temporaries (__tmp*/__str*)
    BlockId start_block;   // Block where variable becomes live (for cleanup records)
    ValueId initial_value; // SSA value at declaration (for cleanup record register mapping)
    OwnedKind kind = OwnedKind::Owned;  // Owned value vs ref borrow vs owned string
};

// Ownership bookkeeping for the IRBuilder: which locals and temporaries own a
// value needing destruction, and their move state. Pure state — all IR
// emission (destroys, Nullify annotations, SSA rebinds) stays in IRBuilder,
// which consults and transitions this tracker. Mirrors the semantic side's
// LifetimeChecker split.
//
// Entries form a stack in declaration order (scope cleanup walks it in
// reverse for LIFO destruction); indices are stable while an entry is present
// because entries are only pushed at the back and popped from the back.
//
// Keyed lookups (replacing the former linear scans; measurements in the
// 2026-07-06 TODO record):
//  - by name: sound because local shadowing is banned
//    (SemanticAnalyzer::check_no_local_shadowing), temporaries get unique
//    interned names, and sequential scopes pop their entries before a name is
//    reused — so at most one co-present entry ever holds a given name.
//  - by value (temporaries only): a ValueId is minted fresh at the producing
//    instruction and tracked at most once, so each value maps to at most one
//    temporary. The mapping is kept through the temp's move (matching the old
//    scans, whose re-track guard also matched moved temps) and dropped only
//    when the entry is popped.
class OwnershipTracker {
public:
    // Clear all state for a new function body. Keeps capacity.
    void reset();

    // Push a tracked entry (declaration order). Registers the keyed lookups.
    void track(const OwnedLocalInfo& info);

    // Drop every entry with scope_depth >= depth (scope exit / function end).
    // Pure bookkeeping: emitting the corresponding cleanup IR (and recording
    // exception cleanup records) is the caller's job, before popping.
    void pop_to_depth(u32 depth);

    // The entry currently bound to `name`, or null. May return a moved entry
    // (reassignment revives it; mark-moved checks is_moved itself).
    OwnedLocalInfo* find_by_name(StringView name);

    // The live temporary produced as `value`, or null (consume sites).
    OwnedLocalInfo* find_live_temp(ValueId value);

    // True when any temporary — moved or not — was tracked for `value`
    // (the re-track guard of track_noncopyable_call_temp / track_string_temp).
    bool has_temp_for(ValueId value) const;

    // Ordered access for the LIFO cleanup walks and cleanup-record emission.
    // Mutating is_moved through entry() is invariant-safe (the keyed maps
    // deliberately retain moved entries); name/value/depth must not change.
    u32 count() const { return m_entries.size(); }
    OwnedLocalInfo& entry(u32 index) { return m_entries[index]; }
    bool empty() const { return m_entries.empty(); }

    // Branch codegen snapshots the is_moved flags before a branch and
    // restores them per branch (see IRBuilder::ScopeSnapshot). Restore is
    // clamped to min(saved, current) — entries pushed and popped inside a
    // branch have no snapshot slot.
    void snapshot_move_state(Vector<bool>& out) const;
    void restore_move_state(const Vector<bool>& saved);

private:
    Vector<OwnedLocalInfo> m_entries;
    tsl::robin_map<StringView, u32> m_index_by_name;
    tsl::robin_map<u32, u32> m_temp_index_by_value;  // ValueId.id -> entry index
};

}

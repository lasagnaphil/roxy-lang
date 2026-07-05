#include "roxy/compiler/ownership_tracker.hpp"

#include <cassert>

namespace rx {

void OwnershipTracker::reset() {
    m_entries.clear_keep_capacity();
    m_index_by_name.clear();        // robin_map::clear keeps capacity
    m_temp_index_by_value.clear();
}

void OwnershipTracker::track(const OwnedLocalInfo& info) {
    u32 index = m_entries.size();
    m_entries.push_back(info);
    // At most one co-present entry per name (shadowing ban + unique temp
    // names + sequential scopes popping before reuse), so this never
    // displaces a live mapping.
    assert(m_index_by_name.find(info.name) == m_index_by_name.end()
           && "duplicate co-present owned-local name (shadowing ban violated?)");
    m_index_by_name[info.name] = index;
    if (info.is_temporary && info.initial_value.is_valid()) {
        m_temp_index_by_value[info.initial_value.id] = index;
    }
}

void OwnershipTracker::pop_to_depth(u32 depth) {
    while (!m_entries.empty() && m_entries.back().scope_depth >= depth) {
        const OwnedLocalInfo& info = m_entries.back();
        m_index_by_name.erase(info.name);
        if (info.is_temporary && info.initial_value.is_valid()) {
            m_temp_index_by_value.erase(info.initial_value.id);
        }
        m_entries.pop_back();
    }
}

OwnedLocalInfo* OwnershipTracker::find_by_name(StringView name) {
    auto it = m_index_by_name.find(name);
    if (it == m_index_by_name.end()) return nullptr;
    return &m_entries[it->second];
}

OwnedLocalInfo* OwnershipTracker::find_live_temp(ValueId value) {
    if (!value.is_valid()) return nullptr;
    auto it = m_temp_index_by_value.find(value.id);
    if (it == m_temp_index_by_value.end()) return nullptr;
    OwnedLocalInfo& info = m_entries[it->second];
    return info.is_moved ? nullptr : &info;
}

bool OwnershipTracker::has_temp_for(ValueId value) const {
    if (!value.is_valid()) return false;
    return m_temp_index_by_value.find(value.id) != m_temp_index_by_value.end();
}

void OwnershipTracker::snapshot_move_state(Vector<bool>& out) const {
    out.reserve(m_entries.size());
    for (const auto& info : m_entries) {
        out.push_back(info.is_moved);
    }
}

void OwnershipTracker::restore_move_state(const Vector<bool>& saved) {
    for (u32 i = 0; i < saved.size() && i < m_entries.size(); i++) {
        m_entries[i].is_moved = saved[i];
    }
}

}

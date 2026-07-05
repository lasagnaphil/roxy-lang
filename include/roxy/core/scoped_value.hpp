#pragma once

#include <utility>

namespace rx {

// RAII guard for an implicit "current context" member. Saves a slot's value on
// construction and restores it on destruction, so a method can freely mutate
// the member and have the previous value reinstated at scope exit — even on an
// early return. Replaces the manual `auto prev = m_x; m_x = ...; ...; m_x =
// prev;` idiom, which silently leaks state if a return slips between the save
// and the restore.
template <typename T>
class ScopedValue {
public:
    explicit ScopedValue(T& slot) : m_slot(slot), m_saved(slot) {}
    ~ScopedValue() { m_slot = std::move(m_saved); }
    ScopedValue(const ScopedValue&) = delete;
    ScopedValue& operator=(const ScopedValue&) = delete;

private:
    T& m_slot;
    T m_saved;
};

}

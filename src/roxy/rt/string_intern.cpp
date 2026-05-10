#include "roxy/rt/roxy_rt.h"
#include "roxy/rt/string_intern.hpp"

extern "C" {

void* roxy_string_intern_lookup(void* table, const char* chars, uint32_t length) {
    if (!table || !chars || length == 0) return nullptr;
    auto* t = static_cast<rx::StringInternTable*>(table);
    rx::StringView key(chars, length);
    auto it = t->table.find(key);
    if (it == t->table.end()) return nullptr;
    return it->second;
}

void roxy_string_intern_insert(void* table, const char* chars, uint32_t length, void* string_obj) {
    if (!table || !chars || length == 0 || !string_obj) return;
    auto* t = static_cast<rx::StringInternTable*>(table);
    // The key's char range must outlive the entry. Callers pass the string
    // object's own chars (stable for the object's lifetime).
    t->table[rx::StringView(chars, length)] = string_obj;
}

} // extern "C"

#pragma once

// Private rt-internal header. The C runtime exposes the intern table via the
// extern-C `roxy_string_intern_lookup` / `roxy_string_intern_insert` declared
// in `roxy_rt.h`; this header defines the actual storage type so VM code that
// owns the table can keep allocating it via `UniquePtr<StringInternTable>`.

#include "roxy/core/string_view.hpp"
#include "roxy/core/tsl/robin_map.h"

namespace rx {

// Content-keyed table used by `roxy_string_from_literal` to dedup heap strings.
// Key is a StringView over the stored string object's char data (stable for
// the object's lifetime, which is the owner's — typically the VM). Value is
// the string data pointer.
struct StringInternTable {
    tsl::robin_map<StringView, void*> table;
};

} // namespace rx

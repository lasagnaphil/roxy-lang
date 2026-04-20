#pragma once

#include "roxy/core/string_view.hpp"
#include "roxy/core/tsl/robin_map.h"

namespace rx {

// VM-owned content-keyed table used by `string_alloc` to dedup heap strings.
// Wrapped in a struct so vm.hpp can forward-declare it instead of pulling in
// the full robin_map implementation on every TU that sees RoxyVM.
struct StringInternTable {
    // Key is a StringView over the stored StringObject's char data (stable
    // for the object's lifetime, which is the VM's lifetime). Value is the
    // StringObject data pointer (the same `void*` string_alloc returns).
    tsl::robin_map<StringView, void*> table;
};

}

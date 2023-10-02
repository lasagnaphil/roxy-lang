#include "roxy/object.hpp"

#include <ctime>

namespace rx {

void init_uid_gen_state() {
    u64 t = time(nullptr);
    xoshiro256ss_init(&tl_uid_gen_state, t);
}

}
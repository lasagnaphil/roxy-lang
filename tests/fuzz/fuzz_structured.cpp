#include "fuzz_targets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    rx::fuzz::fuzz_one_structured(data, size);
    return 0;
}

// libFuzzer entry point for the parser. See tests/fuzz/fuzz_targets.cpp for the
// harness body and tests/fuzz/README.md for how to build and run.
#include "fuzz_targets.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    rx::fuzz::fuzz_one_parser(data, size);
    return 0;
}

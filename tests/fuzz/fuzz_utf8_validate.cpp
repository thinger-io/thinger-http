#include <cstdint>
#include <cstddef>
#include "thinger/http/util/utf8.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    // Exercise the UTF-8 validator with arbitrary input
    thinger::http::utf8::is_valid(data, size);
    return 0;
}

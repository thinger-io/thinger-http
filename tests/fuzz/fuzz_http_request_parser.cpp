#include <cstdint>
#include <cstddef>
#include "thinger/http/server/request_factory.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    thinger::http::request_factory parser;
    auto begin = reinterpret_cast<const char*>(data);
    auto end = begin + size;
    parser.parse(begin, end);
    return 0;
}

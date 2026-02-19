#include <cstdint>
#include <cstddef>
#include <string>
#include <map>
#include "thinger/http/util/url.hpp"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    std::string input(reinterpret_cast<const char*>(data), size);

    thinger::http::util::url::url_decode(input);

    std::multimap<std::string, std::string> params;
    thinger::http::util::url::parse_url_encoded_data(input, params);

    return 0;
}

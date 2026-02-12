#include "websocket_util.hpp"
#include "../../util/base64.hpp"
#include "../../util/sha1.hpp"

#include <random>
#include <regex>
#include <array>

namespace thinger::http::websocket_util {

std::optional<url_components> parse_websocket_url(const std::string& url) {
    std::regex url_regex(R"(^(wss?)://([^/:]+)(?::(\d+))?(/.*)?$)", std::regex::icase);
    std::smatch match;

    if (!std::regex_match(url, match, url_regex)) {
        return std::nullopt;
    }

    url_components result;
    result.scheme = match[1].str();
    result.host = match[2].str();
    result.secure = (result.scheme == "wss" || result.scheme == "WSS");
    result.port = match[3].matched ? match[3].str() : (result.secure ? "443" : "80");
    result.path = match[4].matched ? match[4].str() : "/";

    return result;
}

std::string generate_websocket_key() {
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<> dis(0, 255);

    std::array<uint8_t, 16> bytes;
    for (auto& b : bytes) {
        b = static_cast<uint8_t>(dis(gen));
    }

    return ::thinger::util::base64::encode(bytes.data(), bytes.size());
}

bool validate_accept_key(const std::string& accept_key, const std::string& sent_key) {
    std::string combined = sent_key + WS_GUID;
    std::string sha1_hash = ::thinger::util::sha1::hash(combined);
    std::string expected = ::thinger::util::base64::encode(
        reinterpret_cast<const unsigned char*>(sha1_hash.data()),
        sha1_hash.size()
    );
    return accept_key == expected;
}

} // namespace thinger::http::websocket_util

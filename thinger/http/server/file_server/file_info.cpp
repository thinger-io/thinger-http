#include "file_info.hpp"
#include "../../common/http_request.hpp"
#include "../../util/http_date.hpp"
#include "../../../util/ascii.hpp"
#include <algorithm>
#include <format>
#include <sys/stat.h>

namespace thinger::http {

namespace {

    std::string_view trim_ows(std::string_view value) {
        auto start = value.find_first_not_of(" \t");
        if (start == std::string_view::npos) return {};
        auto end = value.find_last_not_of(" \t");
        return value.substr(start, end - start + 1);
    }

    // A non-negative number of digits only, that fits in 64 bits
    bool parse_position(std::string_view text, uint64_t& value) {
        if (text.empty()) return false;
        value = 0;
        for (char c : text) {
            if (c < '0' || c > '9') return false;
            uint64_t digit = c - '0';
            if (value > (UINT64_MAX - digit) / 10) return false;
            value = value * 10 + digit;
        }
        return true;
    }

}

std::optional<file_info> file_info::of(const std::filesystem::path& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;
#ifdef __APPLE__
    const auto& modified = st.st_mtimespec;
#else
    const auto& modified = st.st_mtim;
#endif
    file_info info;
    info.regular = S_ISREG(st.st_mode);
    info.size = static_cast<uint64_t>(st.st_size);
    info.mtime = modified.tv_sec;
    // nanoseconds: a file rewritten within the same second, with the same size, gets another tag
    auto nanoseconds = static_cast<uint64_t>(modified.tv_sec) * 1'000'000'000 + static_cast<uint64_t>(modified.tv_nsec);
    info.etag = std::format("W/\"{:x}-{:x}\"", info.size, nanoseconds);
    info.last_modified = util::format_http_date(info.mtime);
    return info;
}

bool file_info::not_modified(const http_request& request) const {
    // If-None-Match takes precedence: If-Modified-Since is ignored when it is present.
    // Repeated, its values make one list.
    auto if_none_match = request.get_headers_with_key("If-None-Match");
    if (!if_none_match.empty()) {
        return std::any_of(if_none_match.begin(), if_none_match.end(),
                           [this](const std::string& value) { return etag_matches(value, etag); });
    }

    // Ignored if repeated or not a valid date
    auto if_modified_since = request.get_headers_with_key("If-Modified-Since");
    if (if_modified_since.size() != 1) return false;
    auto since = util::parse_http_date(trim_ows(if_modified_since.front()));
    return since && mtime <= *since;
}

bool etag_matches(std::string_view if_none_match, std::string_view etag) {
    auto opaque = [](std::string_view tag) {
        if (tag.starts_with("W/")) tag.remove_prefix(2);
        return tag;
    };
    auto current = opaque(etag);

    // 1#entity-tag, as W/"opaque" or "opaque" (an opaque tag may hold commas, not quotes)
    std::string_view rest = if_none_match;
    while (true) {
        auto start = rest.find_first_not_of(" \t,");
        if (start == std::string_view::npos) return false;
        rest.remove_prefix(start);
        if (rest.starts_with('*')) return true;
        size_t quote = rest.starts_with("W/") ? 2 : 0;
        if (rest.size() <= quote || rest[quote] != '"') return false;
        auto close = rest.find('"', quote + 1);
        if (close == std::string_view::npos) return false;
        if (rest.substr(quote, close - quote + 1) == current) return true;
        rest.remove_prefix(close + 1);
    }
}

std::optional<byte_range> parse_byte_range(std::string_view range, uint64_t size) {
    range = trim_ows(range);
    constexpr std::string_view unit = "bytes=";
    if (!::thinger::util::ascii::istarts_with(range, unit)) return std::nullopt;
    range = trim_ows(range.substr(unit.size()));

    // Only a single range is served: several of them get the whole resource
    auto dash = range.find('-');
    if (dash == std::string_view::npos || range.find(',') != std::string_view::npos) return std::nullopt;
    auto first_text = range.substr(0, dash);
    auto last_text = range.substr(dash + 1);

    // "-suffix_length": the last bytes
    if (first_text.empty()) {
        uint64_t suffix = 0;
        if (!parse_position(last_text, suffix)) return std::nullopt;
        if (suffix == 0 || size == 0) return byte_range{};
        suffix = std::min(suffix, size);
        return byte_range{size - suffix, suffix};
    }

    // "first-" or "first-last" (a last position past the end is the end)
    uint64_t first = 0;
    uint64_t last = UINT64_MAX;
    if (!parse_position(first_text, first)) return std::nullopt;
    if (!last_text.empty() && (!parse_position(last_text, last) || last < first)) return std::nullopt;
    if (first >= size) return byte_range{};
    last = std::min(last, size - 1);
    return byte_range{first, last - first + 1};
}

} // namespace thinger::http

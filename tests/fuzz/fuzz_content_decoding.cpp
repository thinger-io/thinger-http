// gzip and deflate decoding (Content-Encoding of client responses and server requests), with
// a maximum decoded size taken from the first two bytes of the input (0 to 4 MB) and the rest
// as the compressed data. Each decoder either decodes within the limit or fails, nothing else,
// and the limit only decides whether the data fits: the result is the one decoding without a
// limit gives, too large exactly when that is larger than the limit (or, as decoding stops at
// the limit, when the data is invalid past it), and invalid exactly when that is invalid.
// Decoded data compresses back to itself, and so does the input. gzip decodes every member:
// the input split in two members decodes to itself, and nothing but zeros may follow the
// last member (nor anything a zlib or raw deflate stream). A Content-Encoding list decodes
// as its codings, one after another.

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include "thinger/util/compression.hpp"
#include "fuzz_common.hpp"

namespace {

    using decoder = std::optional<std::string> (*)(const std::string&, size_t, bool*);
    using encoder = std::optional<std::string> (*)(const std::string&);

    void check_decoder(const char* name, decoder decode, encoder encode, const std::string& data, size_t limit) {
        const std::string label = std::string(name) + ", limit " + std::to_string(limit) + ": " + fuzz::escape(data, 64);

        // Without a limit (at most ~1032 bytes per byte of input)
        bool too_large = true;
        auto whole = decode(data, std::numeric_limits<size_t>::max(), &too_large);
        fuzz::check(!too_large, "too large without a limit", label);

        too_large = false;
        auto limited = decode(data, limit, &too_large);
        if (limited) {
            fuzz::check(!too_large, "decoded, and too large", label);
            fuzz::check(limited->size() <= limit, "decoded over the limit", label);
            fuzz::check(whole && *whole == *limited, "decoded with a limit, not the same without it", label);
        } else if (too_large) {
            fuzz::check(!whole || whole->size() > limit, "too large, but fits whole", label);
        } else {
            fuzz::check(!whole, "invalid with a limit, valid without it", label);
        }

        if (!whole) return;

        // Exactly at the limit, and one byte short of it
        too_large = true;
        fuzz::check(decode(data, whole->size(), &too_large) == whole && !too_large, "does not fit its own size", label);
        if (!whole->empty()) {
            fuzz::check(!decode(data, whole->size() - 1, &too_large) && too_large, "fits one byte less than its size", label);
        }

        auto again = encode(*whole);
        fuzz::check(again.has_value(), "decoded data does not compress", label);
        fuzz::check(decode(*again, whole->size(), nullptr) == whole, "decoded data does not round-trip", label);

        // Nothing but padding after the end: a non-zero byte fails (zeros too, but for gzip)
        fuzz::check(!decode(data + "\x01", std::numeric_limits<size_t>::max(), nullptr), "a byte after the end is accepted", label);
        fuzz::check(decode(data + std::string(3, '\0'), std::numeric_limits<size_t>::max(), nullptr).has_value() ==
                    (std::string(name) == "gzip"), "zeros after the end", label);
    }

    std::optional<std::string> gzip_decode(const std::string& data, size_t limit, bool* too_large) {
        return thinger::util::gzip::decompress(data, limit, too_large);
    }

    std::optional<std::string> deflate_decode(const std::string& data, size_t limit, bool* too_large) {
        return thinger::util::deflate::decompress(data, limit, too_large);
    }

    // decode_content() of one coding agrees with its decoder
    void check_content_decoding(const char* codings, decoder decode, const std::string& data, size_t limit) {
        const std::string label = std::string(codings) + ", limit " + std::to_string(limit) + ": " + fuzz::escape(data, 64);
        bool too_large = false;
        auto expected = decode(data, limit, &too_large);
        std::string body = data;
        auto result = thinger::util::decode_content(codings, body, limit);
        if (expected) {
            fuzz::check(result == thinger::util::content_decoding::decoded && body == *expected, "decode_content differs", label);
        } else {
            fuzz::check(result == (too_large ? thinger::util::content_decoding::too_large
                                             : thinger::util::content_decoding::invalid), "decode_content differs", label);
        }
    }

}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size < 2) return 0;
    const size_t limit = ((static_cast<size_t>(data[0]) << 8) | data[1]) * 64;
    const std::string compressed(reinterpret_cast<const char*>(data + 2), size - 2);

    check_decoder("gzip", gzip_decode, thinger::util::gzip::compress, compressed, limit);
    check_decoder("deflate", deflate_decode, thinger::util::deflate::compress, compressed, limit);
    check_content_decoding("X-GZip", gzip_decode, compressed, limit);
    check_content_decoding(" deflate ,identity", deflate_decode, compressed, limit);

    // Any input compresses back to itself: in one gzip member or two, and through two codings
    const std::string input(reinterpret_cast<const char*>(data), size);
    const std::string label = fuzz::escape(input, 64);
    auto gzip = thinger::util::gzip::compress(input);
    auto deflate = thinger::util::deflate::compress(input);
    fuzz::check(gzip && thinger::util::gzip::decompress(*gzip) == input, "gzip round trip", label);
    fuzz::check(deflate && thinger::util::deflate::decompress(*deflate) == input, "deflate round trip", label);

    const size_t split = data[0] % (size + 1);
    auto first = thinger::util::gzip::compress(input.substr(0, split));
    auto second = thinger::util::gzip::compress(input.substr(split));
    fuzz::check(first && second, "gzip members", label);
    bool too_large = false;
    fuzz::check(thinger::util::gzip::decompress(*first + *second, size, &too_large) == input, "gzip members round trip", label);
    fuzz::check(!thinger::util::gzip::decompress(*first + *second, size - 1, &too_large) && too_large,
                "gzip members fit one byte less than their size", label);

    auto stacked = thinger::util::gzip::compress(*deflate);
    fuzz::check(stacked.has_value(), "stacked codings", label);
    std::string body = *stacked;
    auto result = thinger::util::decode_content("deflate, gzip", body, std::numeric_limits<size_t>::max());
    fuzz::check(result == thinger::util::content_decoding::decoded && body == input, "stacked codings round trip", label);
    return 0;
}

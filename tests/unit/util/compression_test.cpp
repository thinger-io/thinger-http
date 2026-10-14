#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/util/compression.hpp>
#include <chrono>
#include <optional>
#include <string>
#include "../../fixtures/compression_bomb.hpp"

using namespace thinger;
using compression_bomb::mib;

// gzip and deflate decoding (client responses and server requests): bounded by a maximum
// size of the decoded data, checked as it is inflated, and failing on anything but a whole,
// valid stream

namespace {

    using compression_bomb::format;

    std::optional<std::string> compress(format type, const std::string& data) {
        return type == format::gzip ? util::gzip::compress(data) : util::deflate::compress(data);
    }

    std::optional<std::string> decompress(format type, const std::string& data, size_t max_size, bool* too_large) {
        return type == format::gzip ? util::gzip::decompress(data, max_size, too_large)
                                    : util::deflate::decompress(data, max_size, too_large);
    }

}

TEST_CASE("Compressed data is decoded up to a maximum size", "[compression][gzip][deflate][unit]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(type == format::gzip ? "gzip" : "deflate");

    const std::string original(100000, 'a');
    auto compressed = compress(type, original);
    REQUIRE(compressed);
    bool too_large = true;

    SECTION("Without a limit") {
        auto decoded = type == format::gzip ? util::gzip::decompress(*compressed) : util::deflate::decompress(*compressed);
        REQUIRE(decoded == original);
    }

    SECTION("Exactly at the limit") {
        auto decoded = decompress(type, *compressed, original.size(), &too_large);
        REQUIRE(decoded == original);
        REQUIRE_FALSE(too_large);
    }

    SECTION("One byte over the limit") {
        auto decoded = decompress(type, *compressed, original.size() - 1, &too_large);
        REQUIRE_FALSE(decoded);
        REQUIRE(too_large);
    }

    SECTION("Empty data compressed") {
        auto empty = compress(type, "");
        REQUIRE(empty);
        REQUIRE(decompress(type, *empty, 0, &too_large) == std::string());
        REQUIRE_FALSE(too_large);
    }
}

TEST_CASE("Compression bombs stop inflating at the limit", "[compression][gzip][deflate][unit]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(type == format::gzip ? "gzip" : "deflate");
    bool too_large = false;

    SECTION("The bomb is a valid stream") {
        auto bomb = compression_bomb::zeros(type, 4);
        REQUIRE(bomb.size() < 8 * 1024);
        auto decoded = decompress(type, bomb, 4 * mib, &too_large);
        REQUIRE(decoded == std::string(4 * mib, '\0'));
    }

    SECTION("A few KB expanding to 1 GiB") {
        auto bomb = compression_bomb::zeros(type, 1024);
        REQUIRE(bomb.size() < 2 * mib);
        auto start = std::chrono::steady_clock::now();
        auto decoded = decompress(type, bomb, 8 * mib, &too_large);
        auto elapsed = std::chrono::steady_clock::now() - start;
        REQUIRE_FALSE(decoded);
        REQUIRE(too_large);
        REQUIRE(elapsed < std::chrono::seconds(2));
    }

    SECTION("Inflating stops as soon as the limit is passed, before the end of the stream") {
        // A broken checksum is only found at the end: past the limit, the data is too large,
        // not invalid, so the end was never reached
        auto bomb = compression_bomb::zeros(type, 16);
        bomb.back() = static_cast<char>(bomb.back() ^ 0x55);
        REQUIRE_FALSE(decompress(type, bomb, mib, &too_large));
        REQUIRE(too_large);
        REQUIRE_FALSE(decompress(type, bomb, 32 * mib, &too_large));
        REQUIRE_FALSE(too_large);
    }
}

TEST_CASE("Malformed compressed data fails to decode", "[compression][gzip][deflate][unit]") {
    auto type = GENERATE(format::gzip, format::deflate);
    CAPTURE(type == format::gzip ? "gzip" : "deflate");

    auto compressed = compress(type, std::string(10000, 'x') + "end");
    REQUIRE(compressed);
    bool too_large = true;

    SECTION("Truncated") {
        for (size_t size : {size_t{0}, size_t{1}, compressed->size() / 2, compressed->size() - 1}) {
            CAPTURE(size);
            REQUIRE_FALSE(decompress(type, compressed->substr(0, size), mib, &too_large));
            REQUIRE_FALSE(too_large);
        }
    }

    SECTION("Not compressed") {
        REQUIRE_FALSE(decompress(type, "plain text, not compressed", mib, &too_large));
        REQUIRE_FALSE(too_large);
    }

    SECTION("Corrupted data") {
        std::string corrupted = *compressed;
        corrupted[corrupted.size() / 2] = static_cast<char>(corrupted[corrupted.size() / 2] ^ 0xff);
        REQUIRE_FALSE(decompress(type, corrupted, mib, &too_large));
        REQUIRE_FALSE(too_large);
    }

    SECTION("The other format") {
        auto other = compress(type == format::gzip ? format::deflate : format::gzip, "data");
        REQUIRE(other);
        REQUIRE_FALSE(decompress(type, *other, mib, &too_large));
        REQUIRE_FALSE(too_large);
    }
}

namespace {

    // Raw deflate (no zlib header nor trailer), as some servers send for "deflate"
    std::string raw_deflate(const std::string& data) {
        auto zlib = util::deflate::compress(data);
        REQUIRE(zlib);
        return zlib->substr(2, zlib->size() - 6);
    }

    std::string gzip_of(const std::string& data) {
        auto compressed = util::gzip::compress(data);
        REQUIRE(compressed);
        return *compressed;
    }

    std::string deflate_of(const std::string& data) {
        auto compressed = util::deflate::compress(data);
        REQUIRE(compressed);
        return *compressed;
    }

}

TEST_CASE("gzip decodes every member of the data", "[compression][gzip][unit]") {
    const std::string first(3000, 'a');
    const std::string second = "second member";
    const auto members = gzip_of(first) + gzip_of(second) + gzip_of("");
    bool too_large = true;

    SECTION("Concatenated members decode to their contents, concatenated") {
        REQUIRE(util::gzip::decompress(members) == first + second);
    }

    SECTION("The limit applies to the whole data") {
        auto total = first.size() + second.size();
        REQUIRE(util::gzip::decompress(members, total, &too_large) == first + second);
        REQUIRE_FALSE(too_large);
        REQUIRE_FALSE(util::gzip::decompress(members, total - 1, &too_large));
        REQUIRE(too_large);
    }

    SECTION("Bombs in several members stop inflating at the limit") {
        auto bombs = compression_bomb::zeros(format::gzip, 4) + compression_bomb::zeros(format::gzip, 4);
        REQUIRE(util::gzip::decompress(bombs, 8 * mib, &too_large) == std::string(8 * mib, '\0'));
        REQUIRE_FALSE(util::gzip::decompress(bombs, 6 * mib, &too_large));
        REQUIRE(too_large);

        std::string many;
        for (int i = 0; i < 64; ++i) many += compression_bomb::zeros(format::gzip, 16);
        auto start = std::chrono::steady_clock::now();
        REQUIRE_FALSE(util::gzip::decompress(many, 8 * mib, &too_large));
        REQUIRE(too_large);
        REQUIRE(std::chrono::steady_clock::now() - start < std::chrono::seconds(2));
    }

    SECTION("Zero bytes after the last member are padding") {
        REQUIRE(util::gzip::decompress(members + std::string(512, '\0')) == first + second);
    }

    SECTION("Other bytes after a member fail") {
        for (const std::string& trailing : {std::string("garbage"), std::string("\n"), std::string(1, '\x1f'),
                                            std::string("\x1f\x8b", 2), std::string("\0\0x", 3),
                                            gzip_of(second).substr(0, 15)}) {
            CAPTURE(trailing);
            REQUIRE_FALSE(util::gzip::decompress(members + trailing, mib, &too_large));
            REQUIRE_FALSE(too_large);
        }
    }

    SECTION("A corrupted second member fails") {
        auto corrupted = gzip_of(first) + gzip_of(second);
        corrupted.back() = static_cast<char>(corrupted.back() ^ 0x01);
        REQUIRE_FALSE(util::gzip::decompress(corrupted, mib, &too_large));
        REQUIRE_FALSE(too_large);
    }
}

TEST_CASE("deflate decodes zlib streams and raw deflate, with nothing after them", "[compression][deflate][unit]") {
    const std::string original = std::string(5000, 'z') + "end";
    bool too_large = true;

    SECTION("Raw deflate, without zlib header") {
        REQUIRE(util::deflate::decompress(raw_deflate(original), mib, &too_large) == original);
        REQUIRE_FALSE(too_large);
        REQUIRE(util::deflate::decompress(raw_deflate("")) == std::string());
    }

    SECTION("Raw deflate bombs stop inflating at the limit") {
        auto zlib = compression_bomb::zeros(format::deflate, 16);
        auto raw = zlib.substr(2, zlib.size() - 6);
        REQUIRE(util::deflate::decompress(raw, 16 * mib, &too_large) == std::string(16 * mib, '\0'));
        REQUIRE_FALSE(util::deflate::decompress(raw, mib, &too_large));
        REQUIRE(too_large);
    }

    SECTION("Truncated raw deflate fails") {
        auto raw = raw_deflate(original);
        REQUIRE_FALSE(util::deflate::decompress(raw.substr(0, raw.size() / 2), mib, &too_large));
        REQUIRE_FALSE(too_large);
    }

    SECTION("Bytes after the end of the stream fail") {
        for (const std::string& trailing : {std::string(1, '\0'), std::string("x"), deflate_of("again")}) {
            CAPTURE(trailing);
            REQUIRE_FALSE(util::deflate::decompress(deflate_of(original) + trailing, mib, &too_large));
            REQUIRE_FALSE(util::deflate::decompress(raw_deflate(original) + trailing, mib, &too_large));
            REQUIRE_FALSE(too_large);
        }
    }

    SECTION("gzip data is not deflate") {
        REQUIRE_FALSE(util::deflate::decompress(gzip_of(original), mib, &too_large));
        REQUIRE_FALSE(too_large);
    }
}

TEST_CASE("Bodies are decoded as their Content-Encoding says", "[compression][gzip][deflate][unit]") {
    using util::content_decoding;
    const std::string original = std::string(2000, 'q') + "end";

    auto decode = [](std::string_view codings, std::string body, size_t max_size = mib) {
        auto result = util::decode_content(codings, body, max_size);
        return std::pair{result, body};
    };

    SECTION("Coding names are case-insensitive, and x-gzip is gzip") {
        for (auto coding : {"gzip", "GZIP", "Gzip", "x-gzip", "X-GZip"}) {
            CAPTURE(coding);
            REQUIRE(decode(coding, gzip_of(original)) == std::pair{content_decoding::decoded, original});
        }
        for (auto coding : {"deflate", "DEFLATE", " Deflate "}) {
            CAPTURE(coding);
            REQUIRE(decode(coding, deflate_of(original)) == std::pair{content_decoding::decoded, original});
        }
    }

    SECTION("Several codings are undone in reverse order") {
        REQUIRE(decode("deflate, gzip", gzip_of(deflate_of(original))) == std::pair{content_decoding::decoded, original});
        REQUIRE(decode("gzip,deflate", deflate_of(gzip_of(original))) == std::pair{content_decoding::decoded, original});
        REQUIRE(decode("gzip, gzip", gzip_of(gzip_of(original))) == std::pair{content_decoding::decoded, original});
        // (in the wrong order, the data is invalid)
        REQUIRE(decode("gzip, deflate", gzip_of(deflate_of(original))).first == content_decoding::invalid);
    }

    SECTION("Identity and empty list elements are no codings") {
        REQUIRE(decode("identity", "plain") == std::pair{content_decoding::decoded, std::string("plain")});
        REQUIRE(decode(", gzip ,, identity", gzip_of(original)) == std::pair{content_decoding::decoded, original});
    }

    SECTION("A coding not supported keeps the body as it is") {
        auto gzipped = gzip_of(original);
        REQUIRE(decode("br", "brotli") == std::pair{content_decoding::unsupported, std::string("brotli")});
        REQUIRE(decode("gzip, br", gzipped) == std::pair{content_decoding::unsupported, gzipped});
        REQUIRE(decode("br, gzip", gzipped) == std::pair{content_decoding::unsupported, gzipped});
        REQUIRE(decode("compress", "data") == std::pair{content_decoding::unsupported, std::string("data")});
    }

    SECTION("At most five codings") {
        std::string body = original;
        std::string codings;
        for (int i = 0; i < 5; ++i) {
            body = gzip_of(body);
            codings += codings.empty() ? "gzip" : ", gzip";
        }
        REQUIRE(decode(codings, body) == std::pair{content_decoding::decoded, original});
        REQUIRE(decode(codings + ", identity", body).first == content_decoding::invalid);
        REQUIRE(decode(codings + ", br", body).first == content_decoding::invalid);
    }

    SECTION("Each step is bounded by the maximum size") {
        REQUIRE(decode("gzip", gzip_of(original), original.size()).first == content_decoding::decoded);
        REQUIRE(decode("gzip", gzip_of(original), original.size() - 1).first == content_decoding::too_large);
        REQUIRE(decode("deflate, gzip", gzip_of(compression_bomb::zeros(format::deflate, 16)), mib).first ==
                content_decoding::too_large);
    }

    SECTION("Invalid data") {
        REQUIRE(decode("gzip", "plain text").first == content_decoding::invalid);
        REQUIRE(decode("gzip", gzip_of(original) + "trailing").first == content_decoding::invalid);
        REQUIRE(decode("x-gzip", "").first == content_decoding::invalid);
    }
}

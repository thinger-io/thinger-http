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

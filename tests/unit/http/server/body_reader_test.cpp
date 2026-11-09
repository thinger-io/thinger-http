#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <thinger/http/server/body_reader.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <algorithm>
#include <cstring>
#include <functional>
#include <limits>
#include <memory>
#include <string>
#include <vector>

using namespace thinger;
using thinger::http::body_error;
using thinger::http::body_reader;
using thinger::http::chunked_decoder;

namespace {

    struct decode_result {
        std::string body;
        size_t consumed = 0;
        bool done = false;
        bool failed = false;
    };

    // Feed `input` to a decoder in the given pieces, with an output buffer of `out_size`
    // bytes, the way body_reader does: same input position until it is consumed
    decode_result decode(const std::string& input, const std::vector<size_t>& pieces, size_t out_size) {
        chunked_decoder decoder;
        decode_result result;
        std::vector<uint8_t> out(out_size);
        auto data = reinterpret_cast<const uint8_t*>(input.data());

        size_t offset = 0;
        for (size_t piece : pieces) {
            size_t end = std::min(offset + piece, input.size());
            while (offset < end && !decoder.done() && !decoder.failed()) {
                size_t produced = 0;
                size_t consumed = decoder.decode(data + offset, end - offset, out.data(), out.size(), produced);
                result.body.append(reinterpret_cast<char*>(out.data()), produced);
                offset += consumed;
                if (consumed == 0 && produced == 0) break;
            }
            if (decoder.done() || decoder.failed()) break;
        }
        result.consumed = offset;
        result.done = decoder.done();
        result.failed = decoder.failed();
        return result;
    }

    // Every way of feeding the input checked: in one piece, split in two at every position,
    // one byte at a time; with a large and a one-byte output buffer
    template<typename Check>
    void for_every_split(const std::string& input, Check check) {
        for (size_t out_size : {size_t{1}, size_t{4096}}) {
            check(decode(input, {input.size()}, out_size));
            for (size_t split = 0; split <= input.size(); split++) {
                check(decode(input, {split, input.size() - split}, out_size));
            }
            check(decode(input, std::vector<size_t>(input.size(), 1), out_size));
        }
    }

    template<typename T>
    T run(thinger::awaitable<T> awaitable) {
        boost::asio::io_context ioc;
        auto result = co_spawn(ioc, std::move(awaitable), boost::asio::use_future);
        ioc.run();
        return result.get();
    }

    // Connection that delivers `data` in reads of at most `piece` bytes
    struct fake_connection {
        std::string data;
        size_t piece;
        size_t position = 0;
        size_t reads = 0;

        body_reader::source source() {
            return [this](uint8_t* buffer, size_t size) -> thinger::awaitable<thinger::io_result> {
                reads++;
                size_t bytes = std::min({size, piece, data.size() - position});
                std::memcpy(buffer, data.data() + position, bytes);
                position += bytes;
                if (bytes == 0) co_return thinger::io_result{boost::asio::error::eof, 0};
                co_return thinger::io_result{boost::system::error_code{}, bytes};
            };
        }

        std::string unread() const { return data.substr(position); }
    };

    std::string as_string(const std::vector<uint8_t>& bytes) {
        return {bytes.begin(), bytes.end()};
    }

    // Connection that delivers `data` in reads of the sizes of `pieces`, in turn, calling
    // `on_read` with the bytes delivered so far before each read
    struct piecewise_connection {
        std::string data;
        std::vector<size_t> pieces;
        std::function<void(size_t)> on_read;
        size_t position = 0;
        size_t reads = 0;

        body_reader::source source() {
            return [this](uint8_t* buffer, size_t size) -> thinger::awaitable<thinger::io_result> {
                if (on_read) on_read(position);
                size_t piece = pieces[reads++ % pieces.size()];
                size_t bytes = std::min({size, piece, data.size() - position});
                std::memcpy(buffer, data.data() + position, bytes);
                position += bytes;
                if (bytes == 0) co_return thinger::io_result{boost::asio::error::eof, 0};
                co_return thinger::io_result{boost::system::error_code{}, bytes};
            };
        }
    };

    // Bytes that differ from one position to the next, so a misplaced piece shows
    std::string patterned(size_t size) {
        std::string data(size, '\0');
        for (size_t i = 0; i < size; ++i) data[i] = static_cast<char>((i * 31 + i / 251) & 0xff);
        return data;
    }

    constexpr size_t KiB = 1024;
    constexpr size_t MiB = 1024 * KiB;

    // The memory a Content-Length body may take once `received` bytes of it arrived: a first
    // piece of 64 KiB, then twice what arrived, over what the body held before. With a 1/64
    // margin for the allocator's rounding (libc++ aligns its capacities, and doubles them so)
    size_t body_memory_bound(size_t received, size_t held) {
        size_t bound = std::max(64 * KiB, 2 * received) + held;
        return bound + bound / 64;
    }

    const std::string next_request = "GET /next HTTP/1.1\r\nHost: localhost\r\n\r\n";
}

// ---------------------------------------------------------------------------
// chunked_decoder
// ---------------------------------------------------------------------------

TEST_CASE("Chunked decoder accepts valid bodies split at any position", "[body_reader][chunked][unit]") {
    struct valid_case {
        std::string encoded;
        std::string body;
    };
    std::vector<valid_case> cases = {
        {"5\r\nhello\r\n0\r\n\r\n", "hello"},
        {"0\r\n\r\n", ""},
        {"5\r\nhello\r\n6\r\n world\r\n0\r\n\r\n", "hello world"},
        {"A\r\n0123456789\r\na\r\nabcdefghij\r\n0\r\n\r\n", "0123456789abcdefghij"},
        {"0000005\r\nhello\r\n000\r\n\r\n", "hello"},
        {"5;name=value\r\nhello\r\n0;a\r\n\r\n", "hello"},
        {"3;x=\"a;b\";y\r\nabc\r\n0;last\r\n\r\n", "abc"},
        {"3;x = y ;z;  w=\"q\" ; v\r\nabc\r\n0\r\n\r\n", "abc"},
        {"3;x=\"a\\\"b\\\\ \t\x80\"\r\nabc\r\n0\r\n\r\n", "abc"},
        {"3;a=b;c=\"\"\r\nabc\r\n0;z=\"y\"\r\n\r\n", "abc"},
        {"0\r\nX-Empty:\r\nX-Spaces:  a b \r\n\r\n", ""},
        {"5\r\nhello\r\n0\r\nX-Trailer: a\r\nOther: b\t c\r\n\r\n", "hello"},
        {"2\r\n\r\n\r\n0\r\n\r\n", "\r\n"},
    };

    for (const auto& c : cases) {
        INFO("encoded: " << c.encoded);
        // Bytes after the body belong to the next request and are never consumed
        std::string input = c.encoded + next_request;
        for_every_split(input, [&](const decode_result& result) {
            REQUIRE(result.done);
            REQUIRE_FALSE(result.failed);
            REQUIRE(result.body == c.body);
            REQUIRE(result.consumed == c.encoded.size());
        });
    }
}

TEST_CASE("Chunked decoder rejects malformed bodies split at any position", "[body_reader][chunked][unit]") {
    std::vector<std::string> cases = {
        "zz\r\nhello\r\n0\r\n\r\n",             // not hex
        "\r\nhello\r\n0\r\n\r\n",               // empty size
        ";ext\r\nhello\r\n0\r\n\r\n",           // empty size with extension
        "-5\r\nhello\r\n0\r\n\r\n",             // sign
        "0x5\r\nhello\r\n0\r\n\r\n",            // prefix
        "5 \r\nhello\r\n0\r\n\r\n",             // whitespace after the size
        " 5\r\nhello\r\n0\r\n\r\n",             // whitespace before the size
        "5\nhello\r\n0\r\n\r\n",                // bare LF after the size
        "5\rhello\r\n0\r\n\r\n",                // CR without LF
        "5\r\nhelloX\r\n0\r\n\r\n",             // data longer than the size
        "5\r\nhello0\r\n\r\n",                  // no CRLF after the data
        "5\r\nhello\n0\r\n\r\n",                // bare LF after the data
        "5;a\nb\r\nhello\r\n0\r\n\r\n",         // LF inside an extension
        "1ffffffffffffffff\r\n",                // size overflow
        "0\r\nX: a\n\r\n",                      // bare LF in a trailer
        "0\r\n\n",                              // bare LF as the final line
        "0\r\n folded\r\n\r\n",                 // obsolete line folding in trailers
        "0\r\n\r\r",                            // final CR not followed by LF
        // Chunk extensions must follow the grammar (found by differential fuzzing)
        "5;\r\nhello\r\n0\r\n\r\n",             // extension without a name
        "5;;a\r\nhello\r\n0\r\n\r\n",           // empty extension
        "5;a@b\r\nhello\r\n0\r\n\r\n",          // separator in the name
        "5;a=\r\nhello\r\n0\r\n\r\n",           // empty value
        "5;a=b c\r\nhello\r\n0\r\n\r\n",        // two tokens as a value
        "5;a \r\nhello\r\n0\r\n\r\n",           // whitespace before the end of the line
        "5;a=b \r\nhello\r\n0\r\n\r\n",         // whitespace after a value at the end
        "5;a=\"b\r\nhello\r\n0\r\n\r\n",        // unterminated quoted string
        "5;a=\"b\"c\r\nhello\r\n0\r\n\r\n",     // characters after a quoted string
        "5;a=\"\x01\"\r\nhello\r\n0\r\n\r\n",   // control character in a quoted string
        "5;a=\"\\\x01\"\r\nhello\r\n0\r\n\r\n", // control character in a quoted pair
        "5;a=\"\r\n\"\r\nhello\r\n0\r\n\r\n",   // CRLF in a quoted string
        "5;a\x7f\r\nhello\r\n0\r\n\r\n",        // DEL in an extension
        "5 ;a\r\nhello\r\n0\r\n\r\n",           // whitespace between the size and ';'
        // Trailer lines must be field lines (found by differential fuzzing: a request
        // following a body that lacks its final CRLF was taken as its trailer section)
        "5\r\nhello\r\n0\r\nGET /smuggled HTTP/1.1\r\nHost: localhost\r\n\r\n",
        "0\r\nX-Trailer\r\n\r\n",               // no colon
        "0\r\n: value\r\n\r\n",                 // empty name
        "0\r\nX Trailer: a\r\n\r\n",            // space in the name
        "0\r\nX-Trailer : a\r\n\r\n",           // space before the colon
        "0\r\nX-Trailer: a\x01\r\n\r\n",        // control character in the value
    };

    for (const auto& input : cases) {
        INFO("encoded: " << input);
        for_every_split(input + next_request, [&](const decode_result& result) {
            REQUIRE(result.failed);
            REQUIRE_FALSE(result.done);
        });
    }
}

TEST_CASE("Chunked decoder limits the size line and the trailer section", "[body_reader][chunked][unit]") {
    SECTION("Extensions up to the limit are skipped") {
        std::string input = "5;" + std::string(chunked_decoder::max_line_size - 4, 'x') + "\r\nhello\r\n0\r\n\r\n";
        auto result = decode(input, {input.size()}, 4096);
        REQUIRE(result.done);
        REQUIRE(result.body == "hello");
    }

    SECTION("Longer extensions are rejected") {
        std::string input = "5;" + std::string(chunked_decoder::max_line_size, 'x') + "\r\nhello\r\n0\r\n\r\n";
        REQUIRE(decode(input, {input.size()}, 4096).failed);
    }

    SECTION("Endless leading zeros are rejected") {
        std::string input = std::string(chunked_decoder::max_line_size + 1, '0') + "5\r\nhello\r\n0\r\n\r\n";
        REQUIRE(decode(input, {input.size()}, 4096).failed);
    }

    SECTION("Trailer section too large") {
        std::string input = "0\r\nX: " + std::string(chunked_decoder::max_trailer_size, 'x') + "\r\n\r\n";
        REQUIRE(decode(input, {input.size()}, 4096).failed);
    }
}

// ---------------------------------------------------------------------------
// body_reader
// ---------------------------------------------------------------------------

TEST_CASE("Chunked body reads leave the following requests in the read-ahead", "[body_reader][chunked][pipelining][unit]") {
    // Small chunked body, then well over the framing batch size of pipelined requests
    std::string following;
    while (following.size() < 3 * body_reader::framing_read_size) following += next_request;
    std::string stream = "5\r\nhello\r\n6;e=1\r\n world\r\n0\r\nT: 1\r\n\r\n" + following;

    SECTION("All of it already in the read-ahead") {
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        std::string body;
        REQUIRE(run(reader.read_all(body, 1024)));
        REQUIRE(body == "hello world");
        REQUIRE_FALSE(reader.has_pending());
        REQUIRE(reader.error() == body_error::none);
        REQUIRE(as_string(reader.take_read_ahead()) == following);
    }

    SECTION("Handler reading three bytes at a time") {
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        std::string body = run([&]() -> thinger::awaitable<std::string> {
            std::string received;
            uint8_t buffer[3];
            while (size_t bytes = co_await reader.read(buffer, sizeof(buffer))) {
                received.append(reinterpret_cast<char*>(buffer), bytes);
            }
            co_return received;
        }());
        REQUIRE(body == "hello world");
        REQUIRE(as_string(reader.take_read_ahead()) == following);
    }

    SECTION("Arriving from the connection in pieces of every size") {
        for (size_t piece : {size_t{1}, size_t{2}, size_t{7}, size_t{512}, size_t{100000}}) {
            INFO("piece: " << piece);
            fake_connection connection{stream, piece};
            body_reader reader;
            reader.set_framing(true, 0);
            reader.set_source(connection.source());

            std::string body;
            REQUIRE(run(reader.read_all(body, 1024)));
            REQUIRE(body == "hello world");
            // Whatever was read past the body is kept, the rest is still unread
            REQUIRE(as_string(reader.take_read_ahead()) + connection.unread() == following);
        }
    }

    SECTION("Partly in the read-ahead, then from the connection") {
        for (size_t split = 0; split <= 40; split++) {
            INFO("split: " << split);
            fake_connection connection{stream.substr(split), 5};
            body_reader reader;
            reader.set_framing(true, 0);
            reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), split);
            reader.set_source(connection.source());

            std::string body;
            REQUIRE(run(reader.read_all(body, 1024)));
            REQUIRE(body == "hello world");
            REQUIRE(as_string(reader.take_read_ahead()) + connection.unread() == following);
        }
    }
}

TEST_CASE("Chunked body errors", "[body_reader][chunked][unit]") {
    SECTION("Malformed framing") {
        std::string stream = "5\r\nhelloXX0\r\n\r\n" + next_request;
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 1024)));
        REQUIRE(reader.error() == body_error::malformed);
        REQUIRE_FALSE(reader.has_pending());
        REQUIRE_FALSE(run(reader.discard(1024)));

        uint8_t buffer[16];
        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
    }

    SECTION("Connection closed before the last chunk") {
        fake_connection connection{"5\r\nhello\r\n6\r\n wo", 3};
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_source(connection.source());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 1024)));
        REQUIRE(reader.error() == body_error::incomplete);
    }

    SECTION("Larger than the limit") {
        std::string stream = "10\r\n0123456789abcdef\r\n0\r\n\r\n";
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 15)));
        REQUIRE(reader.error() == body_error::too_large);
    }

    SECTION("Discarded within the limit") {
        std::string stream = "10\r\n0123456789abcdef\r\n0\r\n\r\n" + next_request;
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        REQUIRE(run(reader.discard(16)));
        REQUIRE(as_string(reader.take_read_ahead()) == next_request);
    }

    SECTION("Discarded over the limit") {
        std::string stream = "10\r\n0123456789abcdef\r\n0\r\n\r\n";
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        REQUIRE_FALSE(run(reader.discard(15)));
    }
}

TEST_CASE("Content-Length body reads", "[body_reader][unit]") {
    SECTION("Read-ahead, then connection, never past the end") {
        fake_connection connection{"world" + next_request, 2};
        body_reader reader;
        reader.set_framing(false, 11);
        std::string ahead = "hello ";
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(ahead.data()), ahead.size());
        reader.set_source(connection.source());

        std::string body;
        REQUIRE(run(reader.read_all(body, 1024)));
        REQUIRE(body == "hello world");
        REQUIRE(connection.unread() == next_request);
        REQUIRE_FALSE(reader.has_pending());
    }

    SECTION("Following request kept in the read-ahead") {
        std::string stream = "hello" + next_request;
        body_reader reader;
        reader.set_framing(false, 5);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(stream.data()), stream.size());

        uint8_t buffer[64];
        REQUIRE(run(reader.read(buffer, sizeof(buffer))) == 5);
        REQUIRE(as_string(reader.take_read_ahead()) == next_request);
    }

    SECTION("Larger than the limit: rejected before reading") {
        fake_connection connection{std::string(100, 'x'), 100};
        body_reader reader;
        reader.set_framing(false, 1000000000000);
        reader.set_source(connection.source());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 1024)));
        REQUIRE(reader.error() == body_error::too_large);
        REQUIRE(connection.reads == 0);
        REQUIRE(body.empty());
    }

    SECTION("Connection closed before the end") {
        fake_connection connection{"hel", 2};
        body_reader reader;
        reader.set_framing(false, 5);
        reader.set_source(connection.source());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 1024)));
        REQUIRE(reader.error() == body_error::incomplete);
        REQUIRE(body == "hel");
        REQUIRE_FALSE(run(reader.discard(1024)));
    }
}

// A head announcing a large body takes memory as the body arrives, not when the head does: a
// client cannot make the server hold far more than it sent
TEST_CASE("Content-Length bodies take memory as their bytes arrive", "[body_reader][limit][unit]") {
    SECTION("Announced, never sent: one first piece at most") {
        piecewise_connection connection{"", {4096}};
        body_reader reader;
        reader.set_framing(false, 8 * MiB);
        reader.set_source(connection.source());

        std::string body;
        size_t held = body.capacity();
        size_t peak = 0;
        connection.on_read = [&](size_t) { peak = std::max(peak, body.capacity()); };
        REQUIRE_FALSE(run(reader.read_all(body, 8 * MiB)));
        REQUIRE(reader.error() == body_error::incomplete);
        CHECK(body.empty());
        CHECK(peak <= body_memory_bound(0, held));
    }

    SECTION("Sent in pieces: twice what arrived, at every read") {
        auto data = patterned(8 * MiB);
        for (size_t piece : {size_t{1000}, size_t{4096}, size_t{65536}, size_t{MiB}}) {
            INFO("piece: " << piece);
            piecewise_connection connection{data, {piece}};
            body_reader reader;
            reader.set_framing(false, data.size());
            reader.set_source(connection.source());

            std::string body;
            size_t held = body.capacity();
            size_t worst_received = 0, worst_capacity = 0;
            bool within = true;
            connection.on_read = [&](size_t received) {
                if (within && body.capacity() > body_memory_bound(received, held)) {
                    within = false;
                    worst_received = received;
                    worst_capacity = body.capacity();
                }
            };
            REQUIRE(run(reader.read_all(body, 8 * MiB)));
            INFO("capacity " << worst_capacity << " with " << worst_received << " bytes received");
            CHECK(within);
            CHECK(body == data);
        }
    }
}

TEST_CASE("Content-Length bodies of every size are read whole, in any pieces", "[body_reader][unit]") {
    const std::vector<std::vector<size_t>> ways = {
        {std::numeric_limits<size_t>::max()},   // whole
        {7},
        {4096},
        {1, 4095, 13, 65536, 777, 100003},      // arbitrary
    };
    for (size_t size : {size_t{0}, size_t{1}, 64 * KiB, 64 * KiB + 1, 8 * MiB}) {
        auto data = patterned(size);
        auto pieces = ways;
        if (size <= 64 * KiB + 1) pieces.push_back({1});
        for (const auto& way : pieces) {
            INFO("size " << size << ", first piece " << way.front());
            piecewise_connection connection{data, way};
            body_reader reader;
            reader.set_framing(false, size);
            reader.set_source(connection.source());

            std::string body;
            REQUIRE(run(reader.read_all(body, 8 * MiB)));
            CHECK(reader.error() == body_error::none);
            CHECK_FALSE(reader.has_pending());
            CHECK(body == data);
        }

        // Its start in the read-ahead (read with the head), the rest from the connection
        size_t ahead = std::min<size_t>(size, 1000);
        piecewise_connection connection{data.substr(ahead), {4096}};
        body_reader reader;
        reader.set_framing(false, size);
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(data.data()), ahead);
        reader.set_source(connection.source());

        std::string body;
        INFO("size " << size << ", " << ahead << " bytes read ahead");
        REQUIRE(run(reader.read_all(body, 8 * MiB)));
        CHECK(body == data);
    }
}

TEST_CASE("Content-Length bodies are appended to what the body held", "[body_reader][unit]") {
    auto data = patterned(200 * KiB);
    piecewise_connection connection{data, {4096}};
    body_reader reader;
    reader.set_framing(false, data.size());
    reader.set_source(connection.source());

    std::string body = "already here:";
    REQUIRE(run(reader.read_all(body, 8 * MiB)));
    CHECK(body == "already here:" + data);
}

TEST_CASE("Content-Length bodies cut short keep the bytes received", "[body_reader][unit]") {
    // Within the first piece, at its end, and past it
    for (size_t sent : {size_t{1000}, 64 * KiB, 64 * KiB + 1, 300 * KiB}) {
        INFO("sent " << sent);
        auto data = patterned(sent);
        piecewise_connection connection{data, {4096}};
        body_reader reader;
        reader.set_framing(false, 1 * MiB);
        reader.set_source(connection.source());

        std::string body;
        REQUIRE_FALSE(run(reader.read_all(body, 8 * MiB)));
        CHECK(reader.error() == body_error::incomplete);
        CHECK(body == data);
    }
}

TEST_CASE("Content-Length bodies over the limit take no memory", "[body_reader][limit][unit]") {
    piecewise_connection connection{std::string(100, 'x'), {100}};
    body_reader reader;
    reader.set_framing(false, 8 * MiB + 1);
    reader.set_source(connection.source());

    std::string body;
    size_t held = body.capacity();
    REQUIRE_FALSE(run(reader.read_all(body, 8 * MiB)));
    CHECK(reader.error() == body_error::too_large);
    CHECK(connection.reads == 0);
    CHECK(body.capacity() == held);
}

TEST_CASE("Body size limit of every read", "[body_reader][limit][unit]") {
    uint8_t buffer[64];

    SECTION("Content-Length over the limit: the first read fails before reading") {
        fake_connection connection{std::string(100, 'x'), 100};
        body_reader reader;
        reader.set_framing(false, 100);
        reader.set_limit(99);
        reader.set_source(connection.source());

        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
        REQUIRE(reader.error() == body_error::too_large);
        REQUIRE(connection.reads == 0);
        REQUIRE_FALSE(reader.has_pending());
    }

    SECTION("Content-Length within the limit") {
        fake_connection connection{std::string(100, 'x'), 30};
        body_reader reader;
        reader.set_framing(false, 100);
        reader.set_limit(100);
        reader.set_source(connection.source());

        size_t total = 0;
        while (size_t bytes = run(reader.read_some(buffer, sizeof(buffer)))) total += bytes;
        REQUIRE(total == 100);
        REQUIRE(reader.error() == body_error::none);
    }

    SECTION("Chunked: fails once the body exceeds the limit") {
        std::string body;
        for (int i = 0; i < 10; ++i) body += "a\r\n0123456789\r\n";
        fake_connection connection{body + "0\r\n\r\n", 7};
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_limit(45);
        reader.set_source(connection.source());

        size_t total = 0;
        while (size_t bytes = run(reader.read_some(buffer, sizeof(buffer)))) total += bytes;
        REQUIRE(total <= 45);
        REQUIRE(reader.error() == body_error::too_large);
        REQUIRE_FALSE(run(reader.discard(1024)));
    }

    SECTION("Chunked within the limit, and discarded") {
        fake_connection connection{"a\r\n0123456789\r\n0\r\n\r\n" + next_request, 5};
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_limit(10);
        reader.set_source(connection.source());

        REQUIRE(run(reader.discard(1024)));
        REQUIRE(reader.error() == body_error::none);
    }
}

TEST_CASE("Cancelled body reads leave the body pending", "[body_reader][cancellation][unit]") {
    // A connection whose data has not arrived yet: a read waits until it is cancelled (0
    // bytes, as a socket read cancelled), then data arrives
    boost::asio::io_context ioc;
    std::string data;
    auto source = [&](uint8_t* buffer, size_t size) -> thinger::awaitable<thinger::io_result> {
        if (data.empty()) {
            boost::asio::steady_timer never(ioc, std::chrono::steady_clock::time_point::max());
            auto [ec] = co_await never.async_wait(thinger::use_nothrow_awaitable);
            co_return thinger::io_result{ec, 0};
        }
        size_t bytes = std::min(size, data.size());
        std::memcpy(buffer, data.data(), bytes);
        data.erase(0, bytes);
        co_return thinger::io_result{boost::system::error_code{}, bytes};
    };

    // Reads with a timeout, as a handler or the reverse proxy do (the read loses the race)
    auto read_with_timeout = [&](body_reader& reader) {
        auto read = [&]() -> thinger::awaitable<size_t> {
            uint8_t buffer[64];
            boost::asio::steady_timer timer(ioc, std::chrono::milliseconds(10));
            auto result = co_await (reader.read_some(buffer, sizeof(buffer)) ||
                                    timer.async_wait(thinger::use_nothrow_awaitable));
            co_return result.index() == 0 ? std::get<0>(result) : 0;
        };
        auto result = co_spawn(ioc, read(), boost::asio::use_future);
        ioc.restart();
        ioc.run();
        return result.get();
    };

    uint8_t buffer[64];

    SECTION("Content-Length") {
        body_reader reader;
        reader.set_framing(false, 5);
        reader.set_source(source);

        REQUIRE(read_with_timeout(reader) == 0);
        REQUIRE(reader.error() == body_error::none);
        REQUIRE(reader.has_pending());

        data = "hello";
        REQUIRE(run(reader.read(buffer, sizeof(buffer))) == 5);
        REQUIRE_FALSE(reader.has_pending());
    }

    SECTION("Chunked, waiting for the framing") {
        body_reader reader;
        reader.set_framing(true, 0);
        reader.set_source(source);

        REQUIRE(read_with_timeout(reader) == 0);
        REQUIRE(reader.error() == body_error::none);
        REQUIRE(reader.has_pending());

        data = "5\r\nhello\r\n0\r\n\r\n";
        std::string body;
        REQUIRE(run(reader.read_all(body, 1024)));
        REQUIRE(body == "hello");
    }

    SECTION("Chunked, waiting for chunk data") {
        body_reader reader;
        reader.set_framing(true, 0);
        std::string ahead = "5\r\n";
        reader.set_read_ahead(reinterpret_cast<const uint8_t*>(ahead.data()), ahead.size());
        reader.set_source(source);

        REQUIRE(read_with_timeout(reader) == 0);
        REQUIRE(reader.error() == body_error::none);
        REQUIRE(reader.has_pending());

        data = "hello\r\n0\r\n\r\n";
        std::string body;
        REQUIRE(run(reader.read_all(body, 1024)));
        REQUIRE(body == "hello");
    }

    SECTION("A connection that ends is still an incomplete body") {
        body_reader reader;
        reader.set_framing(false, 5);
        reader.set_source([](uint8_t*, size_t) -> thinger::awaitable<thinger::io_result> {
            co_return thinger::io_result{boost::asio::error::eof, 0};
        });

        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
        REQUIRE(reader.error() == body_error::incomplete);
    }
}

TEST_CASE("A read ended by the read deadline is a timeout, nothing else", "[http][body_reader][deadline]") {
    uint8_t buffer[16];
    body_reader reader;
    reader.set_framing(GENERATE(false, true), 100);

    SECTION("At the deadline: 0, the timeout error, and no other read") {
        size_t reads = 0;
        reader.set_source([&](uint8_t*, size_t) -> thinger::awaitable<io_result> {
            ++reads;
            co_return io_result{boost::system::errc::make_error_code(http::read_deadline_expired), 0};
        });
        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
        CHECK(reader.error() == body_error::timeout);
        CHECK(reads == 1);
        CHECK_FALSE(reader.has_pending());
        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
        CHECK(reads == 1);
        CHECK(reader.error() == body_error::timeout);
    }

    // A TCP connection that timed out is the client gone, not the deadline of the handler
    SECTION("An ETIMEDOUT from the socket is still an incomplete body") {
        reader.set_source([](uint8_t*, size_t) -> thinger::awaitable<io_result> {
            co_return io_result{boost::asio::error::timed_out, 0};
        });
        REQUIRE(run(reader.read_some(buffer, sizeof(buffer))) == 0);
        CHECK(reader.error() == body_error::incomplete);
    }
}

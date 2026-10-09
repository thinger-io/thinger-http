#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/body_reader.hpp>
#include <boost/asio.hpp>
#include <boost/asio/use_future.hpp>
#include <algorithm>
#include <cstring>
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
            return [this](uint8_t* buffer, size_t size) -> thinger::awaitable<size_t> {
                reads++;
                size_t bytes = std::min({size, piece, data.size() - position});
                std::memcpy(buffer, data.data() + position, bytes);
                position += bytes;
                co_return bytes;
            };
        }

        std::string unread() const { return data.substr(position); }
    };

    std::string as_string(const std::vector<uint8_t>& bytes) {
        return {bytes.begin(), bytes.end()};
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

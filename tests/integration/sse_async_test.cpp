#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/use_future.hpp>

#include "server_test_helpers.hpp"

#include <future>
#include <map>
#include <string>
#include <thread>
#include <vector>

// Events sent with send_async() to an sse_connection on a strand of an io_context run by several
// threads, from coroutines on other strands: written whole and in order, each producer told once
// written, or false once the connection closed. Run under TSan, these tests report the race of
// a connection that runs its work on the io_context instead of its executor

using namespace thinger;
using namespace std::chrono_literals;
using tcp = boost::asio::ip::tcp;

namespace {

    constexpr int THREADS = 4;

    // An sse_connection on a strand of an io_context run by THREADS threads, and its raw client
    // (the stream from its first byte: no HTTP request nor response)
    struct sse_on_strand {
        boost::asio::io_context context;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work{context.get_executor()};
        tcp::acceptor acceptor{context, {boost::asio::ip::make_address("127.0.0.1"), 0}};
        http::test::raw_connection client{http::test::transport_endpoint{
            http::test::transport::tcp, acceptor.local_endpoint().port(), {}}};
        boost::asio::strand<boost::asio::io_context::executor_type> strand = boost::asio::make_strand(context);
        std::shared_ptr<http::sse_connection> sse;
        std::vector<std::thread> threads;

        sse_on_strand() {
            auto socket = std::make_shared<asio::tcp_socket>("sse", acceptor.accept());
            sse = std::make_shared<http::sse_connection>(std::move(socket), strand);
            // nothing on the wire but the events
            sse->set_keepalive(0ms);
            sse->start();
            for (int t = 0; t < THREADS; ++t) threads.emplace_back([this] { context.run_for(30s); });
        }

        ~sse_on_strand() {
            sse->stop();
            work.reset();
            for (auto& thread : threads) thread.join();
        }

        // Send `event` from a coroutine on a strand of its own: whether it was written
        std::future<bool> send_async(http::sse_event event) {
            return co_spawn(boost::asio::make_strand(context), sse->send_async(std::move(event)), boost::asio::use_future);
        }

        // Read until the stream ends with `last` (megabytes may come before: not searched)
        std::string read_until_last(std::string_view last, std::chrono::milliseconds timeout = 20s) {
            std::string data;
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!data.ends_with(last) && !client.eof()) {
                auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
                if (client.read_some(data, left) == 0 && !client.eof()) break;
            }
            return data;
        }
    };

    http::sse_event event_of(int producer, int index) {
        return {.event = "p" + std::to_string(producer),
                .id = std::to_string(producer) + "-" + std::to_string(index),
                .data = std::to_string(producer) + " " + std::to_string(index) + "\nsecond line"};
    }

    std::string wire_of(int producer, int index) {
        auto p = std::to_string(producer);
        auto i = std::to_string(index);
        return "event: p" + p + "\nid: " + p + "-" + i + "\ndata: " + p + " " + i + "\ndata: second line\n\n";
    }

    // Send `events` events, one after the other, each awaited
    awaitable<std::vector<bool>> produce(std::shared_ptr<http::sse_connection> sse, int producer, int events) {
        std::vector<bool> written;
        for (int i = 0; i < events; ++i) written.push_back(co_await sse->send_async(event_of(producer, i)));
        co_return written;
    }

    // Whether `stream` is the events of every producer, each one whole and in the order sent
    // (those of different producers interleaved in any way)
    bool whole_and_in_order(const std::string& stream, int producers, int events) {
        std::map<int, int> next;
        size_t pos = 0;
        while (pos < stream.size()) {
            auto end = stream.find("\n\n", pos);
            if (end == std::string::npos || stream.compare(pos, 8, "event: p") != 0) return false;
            int producer = std::stoi(stream.substr(pos + 8));
            auto expected = wire_of(producer, next[producer]++);
            if (stream.compare(pos, end + 2 - pos, expected) != 0) return false;
            pos = end + 2;
        }
        if (static_cast<int>(next.size()) != producers) return false;
        for (auto [producer, sent] : next) {
            if (sent != events) return false;
        }
        return true;
    }

    // Queue more than the socket buffers take (the client does not read): what is sent after
    // it waits to be written
    void congest(http::sse_connection& sse) {
        sse.set_write_limits({.max_messages = 0});
        for (int i = 0; i < 400; ++i) sse.send(http::sse_event{.data = std::string(64 * 1024, 'x')});
    }

}

TEST_CASE("SSE send_async from strands of an io_context run by several threads",
          "[sse][executor][threading][integration]") {
    constexpr int producers = 4;
    constexpr int events = 200;
    sse_on_strand on_strand;
    CHECK(on_strand.sse->get_executor() == boost::asio::any_io_executor(on_strand.strand));

    std::vector<std::future<std::vector<bool>>> results;
    for (int p = 0; p < producers; ++p) {
        results.push_back(co_spawn(boost::asio::make_strand(on_strand.context), produce(on_strand.sse, p, events),
                                   boost::asio::use_future));
    }
    for (auto& result : results) {
        REQUIRE(result.wait_for(20s) == std::future_status::ready);
        CHECK(result.get() == std::vector<bool>(events, true));
    }

    // each one written once told: all in the stream, whole and in order per producer
    on_strand.sse->stop();
    auto stream = on_strand.client.read_until_closed(10s);
    REQUIRE(stream.closed);
    CHECK(server_test::count(stream.data, "\n\n") == producers * events);
    CHECK(whole_and_in_order(stream.data, producers, events));

    // closed: refused at once
    auto late = on_strand.send_async(event_of(0, events));
    REQUIRE(late.wait_for(5s) == std::future_status::ready);
    CHECK_FALSE(late.get());
}

TEST_CASE("SSE send_async waits until written, and is told false once closed",
          "[sse][executor][threading][integration]") {
    sse_on_strand on_strand;
    congest(*on_strand.sse);
    auto awaited = on_strand.send_async(http::sse_event{.event = "last", .data = "awaited"});
    // the client does not read: not written yet
    REQUIRE(awaited.wait_for(300ms) == std::future_status::timeout);

    SECTION("true once written") {
        // after everything sent before it
        auto stream = on_strand.read_until_last("\n\nevent: last\ndata: awaited\n\n");
        REQUIRE(stream.ends_with("\n\nevent: last\ndata: awaited\n\n"));
        CHECK(server_test::count(stream, "\n\n") == 401);
        REQUIRE(awaited.wait_for(5s) == std::future_status::ready);
        CHECK(awaited.get());
    }

    SECTION("false when stopped while waiting") {
        on_strand.sse->stop();
        REQUIRE(awaited.wait_for(5s) == std::future_status::ready);
        CHECK_FALSE(awaited.get());
        auto late = on_strand.send_async(http::sse_event{.data = "late"});
        REQUIRE(late.wait_for(5s) == std::future_status::ready);
        CHECK_FALSE(late.get());
    }

    SECTION("false when the client leaves while waiting") {
        on_strand.client.reset();
        REQUIRE(awaited.wait_for(5s) == std::future_status::ready);
        CHECK_FALSE(awaited.get());
    }
}

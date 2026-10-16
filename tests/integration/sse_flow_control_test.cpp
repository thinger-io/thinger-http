#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/sse_connection.hpp>

#include "server_test_helpers.hpp"

#include <future>
#include <mutex>
#include <vector>

// Flow control of the server sse_connection: queued bytes and watermarks, a producer waiting
// for the queue to drain, and the overflow policies

using namespace thinger;
using namespace std::chrono_literals;
using server_test::running_server;

namespace {

    namespace net = boost::asio;
    using tcp = net::ip::tcp;
    using sse_ptr = std::shared_ptr<http::sse_connection>;

    template<typename F>
    bool eventually(F condition, std::chrono::milliseconds timeout = 10s) {
        auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!condition()) {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(5ms);
        }
        return true;
    }

    // Raw SSE client: the response headers, then the stream as received
    class sse_client {
    public:
        explicit sse_client(uint16_t port, const std::string& path = "/events") : sock_(ioc_) {
            sock_.connect({net::ip::make_address("127.0.0.1"), port});
            std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\nAccept: text/event-stream\r\n\r\n";
            net::write(sock_, net::buffer(request));
        }

        // Read until `events` events (empty lines) are received, the connection closes, or the
        // timeout expires: the stream after the headers
        std::string read_events(size_t events, std::chrono::milliseconds timeout = 1s) {
            auto deadline = std::chrono::steady_clock::now() + timeout;
            while (!closed_ && events_ < events && read_more(deadline)) scan();
            scan();
            return body_ == std::string::npos ? std::string{} : data_.substr(body_);
        }

        bool closed() const { return closed_; }

    private:
        // Count the events received since the last scan
        void scan() {
            if (body_ == std::string::npos) {
                auto end = data_.find("\r\n\r\n");
                if (end == std::string::npos) return;
                body_ = scanned_ = end + 4;
            }
            for (auto pos = data_.find("\n\n", scanned_); pos != std::string::npos; pos = data_.find("\n\n", pos + 2)) {
                ++events_;
                scanned_ = pos + 2;
            }
            // a line feed at the end may pair with the next one received
            if (!data_.empty()) scanned_ = std::max(scanned_, data_.size() - 1);
        }

        bool read_more(std::chrono::steady_clock::time_point deadline) {
            auto timeout = deadline - std::chrono::steady_clock::now();
            if (timeout <= std::chrono::steady_clock::duration::zero()) return false;
            bool done = false;
            sock_.async_read_some(net::buffer(chunk_), [&](boost::system::error_code ec, size_t bytes) {
                data_.append(chunk_.data(), bytes);
                if (ec) closed_ = true;
                done = true;
            });
            ioc_.restart();
            ioc_.run_for(timeout);
            if (!done) {
                sock_.cancel();
                ioc_.restart();
                ioc_.run();
                return false;
            }
            return !closed_;
        }

        net::io_context ioc_;
        tcp::socket sock_;
        std::array<char, 65536> chunk_{};
        std::string data_;
        size_t body_ = std::string::npos;
        size_t scanned_ = 0;
        size_t events_ = 0;
        bool closed_ = false;
    };

    std::string data_events(size_t from, size_t to) {
        std::string stream;
        for (auto i = from; i < to; ++i) stream += "data: " + std::to_string(i) + "\n\n";
        return stream;
    }

    struct producer_state {
        std::atomic<size_t> sent{0};
        std::atomic<size_t> max_queued{0};
        std::atomic<bool> failed{false};
        std::atomic<bool> done{false};
        std::promise<sse_ptr> created;
    };

    // The producer is stalled: what it sent and the server queue (read from this thread:
    // queued_bytes() is atomic) unchanged over `samples` polls in a row, with something queued
    // up to `bound`
    bool stalled(const producer_state& state, const sse_ptr& sse, size_t bound, int samples = 20) {
        size_t last_sent = SIZE_MAX;
        size_t last_queued = SIZE_MAX;
        int same = 0;
        return eventually([&] {
            auto sent = state.sent.load();
            auto queued = sse->queued_bytes();
            bool stuck = sent == last_sent && queued == last_queued && queued > 0 && queued <= bound;
            same = stuck ? same + 1 : 0;
            last_sent = sent;
            last_queued = queued;
            return same >= samples;
        });
    }

    // Long data with its index first
    std::string payload(size_t index, size_t size) {
        auto data = std::to_string(index) + ":";
        data.resize(size, 'x');
        return data;
    }

    awaitable<void> producer(sse_ptr sse, std::shared_ptr<producer_state> state, size_t count, size_t size) {
        for (size_t i = 0; i < count; ++i) {
            sse->send_data(payload(i, size));
            state->max_queued = std::max(state->max_queued.load(), sse->queued_bytes());
            if (!co_await sse->wait_writable()) state->failed = true;
            state->sent = i + 1;
        }
        state->done = true;
    }

}

TEST_CASE("SSE overflow policies", "[sse][flow][integration]") {
    http::server server;
    std::function<void(const sse_ptr&)> setup;
    std::promise<void> handled;
    server.get("/events", [&](http::request&, http::response& res) {
        res.start_sse([&](sse_ptr sse) {
            setup(sse);
            handled.set_value();
        });
    });
    running_server running(server);
    // connected once `setup` is set: the request is sent right away
    auto connect = [&] { return std::make_unique<sse_client>(running.port); };

    SECTION("drop_newest by default: 101 fields") {
        http::write_limits limits;
        setup = [&](const sse_ptr& sse) {
            limits = sse->get_write_limits();
            // queued at once (the first one may be being written, still counted)
            for (size_t i = 0; i < 150; ++i) sse->send_data(std::to_string(i));
        };
        REQUIRE(connect()->read_events(150) == data_events(0, 101));
        handled.get_future().get();
        REQUIRE(limits.max_messages == 101);
        REQUIRE(limits.policy == http::overflow_policy::drop_newest);
    }

    SECTION("drop_oldest drops whole events") {
        size_t queued = 0;
        setup = [&](const sse_ptr& sse) {
            sse->set_write_limits({.max_messages = 10, .policy = http::overflow_policy::drop_oldest});
            for (size_t i = 0; i < 30; ++i) {
                sse->send_event("e" + std::to_string(i));
                sse->send_data(std::to_string(i));
            }
            queued = sse->queued_messages();
        };
        // the first event may be being written as the others are queued (co_spawn starts the
        // writer inline since Boost 1.80): then it is completed, and the oldest whole events
        // after it are dropped
        auto event = [](size_t i) { return "event: e" + std::to_string(i) + "\ndata: " + std::to_string(i) + "\n\n"; };
        std::string last_five, in_flight = event(0);
        for (size_t i = 25; i < 30; ++i) last_five += event(i);
        for (size_t i = 26; i < 30; ++i) in_flight += event(i);
        auto stream = connect()->read_events(30);
        REQUIRE((stream == last_five || stream == in_flight));
        handled.get_future().get();
        REQUIRE(queued == 10);
    }

    SECTION("drop_newest with a byte limit") {
        setup = [&](const sse_ptr& sse) {
            // "data: N\n\n": 9 bytes each
            sse->set_write_limits({.max_messages = 0, .max_bytes = 30});
            for (size_t i = 0; i < 5; ++i) sse->send_data(std::to_string(i));
        };
        REQUIRE(connect()->read_events(5) == data_events(0, 3));
    }

    SECTION("close") {
        setup = [&](const sse_ptr& sse) {
            sse->set_write_limits({.max_messages = 4, .policy = http::overflow_policy::close});
            for (size_t i = 0; i < 5; ++i) sse->send_data(std::to_string(i));
        };
        // at most the field being written as the others were queued
        auto client = connect();
        auto stream = client->read_events(5, 3s);
        REQUIRE((stream.empty() || stream == "data: 0\n\n"));
        REQUIRE(client->closed());
    }
}

TEST_CASE("SSE watermarks and queued bytes", "[sse][flow][integration]") {
    http::server server;
    std::mutex mutex;
    std::vector<std::string> events;
    std::promise<sse_ptr> created;
    server.get("/events", [&](http::request&, http::response& res) {
        res.start_sse([&](sse_ptr sse) {
            sse->set_write_limits({.max_messages = 0, .high_watermark = 500});
            sse->on_high_watermark([&, sse] {
                std::lock_guard lock(mutex);
                events.push_back("high " + std::to_string(sse->queued_bytes()));
            });
            sse->on_low_watermark([&, sse] {
                std::lock_guard lock(mutex);
                events.push_back("low " + std::to_string(sse->queued_bytes()));
            });
            // "data: " + 100 bytes + "\n\n": 108 bytes each
            for (size_t i = 0; i < 10; ++i) sse->send_data(std::string(100, 'x'));
            std::lock_guard lock(mutex);
            events.push_back("queued " + std::to_string(sse->queued_bytes()) + " " +
                             std::to_string(sse->queued_messages()));
            created.set_value(sse);
        });
    });
    running_server running(server);

    sse_client client(running.port);
    auto stream = client.read_events(10);
    REQUIRE(server_test::count(stream, "\n\n") == 10);
    auto sse = created.get_future().get();
    REQUIRE(eventually([&] {
        std::lock_guard lock(mutex);
        return events.size() == 3;
    }));
    std::lock_guard lock(mutex);
    // everything queued at once; high told afterwards (not from a send), then low, once each
    REQUIRE(events[0] == "queued 1080 10");
    REQUIRE(events[1].starts_with("high "));
    REQUIRE(events[2] == "low 0");
    sse->stop();
}

TEST_CASE("SSE producer waiting for the queue to drain", "[sse][flow][integration]") {
    constexpr size_t count = 400;
    constexpr size_t size = 64 * 1024;
    constexpr size_t high = 1024 * 1024;

    http::server server;
    auto state = std::make_shared<producer_state>();
    server.get("/events", [state](http::request&, http::response& res) {
        res.start_sse([state](sse_ptr sse) -> awaitable<void> {
            sse->set_write_limits({.max_messages = 0, .high_watermark = high, .low_watermark = high / 4});
            state->created.set_value(sse);
            co_spawn(co_await boost::asio::this_coro::executor, producer(sse, state, count, size), detached);
        });
    });
    running_server running(server);

    sse_client client(running.port);
    // The client does not read: the producer waits once the socket buffers and the queue fill,
    // with the field that reached the high watermark ("data: " + payload + "\n\n" each)
    auto sse = state->created.get_future().get();
    REQUIRE(stalled(*state, sse, 16 * (size + 8)));
    REQUIRE(state->sent.load() < count);

    auto stream = client.read_events(count, 10s);
    REQUIRE(eventually([&] { return state->done.load(); }));
    REQUIRE_FALSE(state->failed.load());
    // each field is "data: " + the payload + "\n\n": the one reaching the high watermark
    // goes past it
    REQUIRE(state->max_queued.load() <= high + size + 8);
    REQUIRE(stream.size() == count * (size + 8));
    for (size_t i = 0; i < count; ++i) {
        auto expected = "data: " + std::to_string(i) + ":";
        REQUIRE(stream.compare(i * (size + 8), expected.size(), expected) == 0);
    }
}

TEST_CASE("SSE drop_oldest never grows past the limits", "[sse][flow][integration]") {
    // fields that never end an event cannot be dropped as whole events: the newest goes
    http::server server;
    std::promise<size_t> queued;
    server.get("/events", [&](http::request&, http::response& res) {
        res.start_sse([&](sse_ptr sse) {
            sse->set_write_limits({.max_messages = 5, .policy = http::overflow_policy::drop_oldest});
            for (size_t i = 0; i < 20; ++i) sse->send_event("e" + std::to_string(i));
            queued.set_value(sse->queued_messages());
        });
    });
    running_server running(server);
    sse_client client(running.port);
    auto stream = client.read_events(1);
    REQUIRE(queued.get_future().get() == 5);
    REQUIRE(stream == "event: e0\nevent: e1\nevent: e2\nevent: e3\nevent: e4\n");
}

TEST_CASE("SSE watermark callbacks that send", "[sse][flow][integration]") {
    http::server server;
    std::promise<int> lows_in_handler;
    std::promise<std::weak_ptr<http::sse_connection>> created;
    auto lows = std::make_shared<std::atomic<int>>(0);
    std::function<void(const sse_ptr&)> setup;
    server.get("/events", [&](http::request&, http::response& res) {
        res.start_sse([&](sse_ptr sse) {
            sse->on_low_watermark([lows, weak = std::weak_ptr(sse)] {
                ++*lows;
                // re-enters the queue: never while it is being changed
                if (auto sse = weak.lock()) {
                    for (int i = 0; i < 4000; ++i) sse->send_data("x");
                }
            });
            setup(sse);
            lows_in_handler.set_value(lows->load());
            created.set_value(sse);
        });
    });
    running_server running(server);
    auto connect = [&] { return std::make_unique<sse_client>(running.port); };

    SECTION("during drop_oldest") {
        setup = [](const sse_ptr& sse) {
            sse->set_write_limits({.max_messages = 0, .max_bytes = 640 << 10, .high_watermark = 500 << 10,
                                   .low_watermark = 450 << 10, .policy = http::overflow_policy::drop_oldest});
            for (int i = 0; i < 10; ++i) sse->send_data(std::string(64 * 1024, 'y'));
            // drops five events: below the low watermark
            sse->send_data(std::string(300 * 1024, 'z'));
        };
        auto client = connect();
        REQUIRE(lows_in_handler.get_future().get() == 0);
        // told afterwards; what it sends arrives
        REQUIRE(eventually([&] { return lows->load() >= 1; }));
        auto stream = client->read_events(4006, 5s);
        REQUIRE(server_test::count(stream, "data: x\n\n") >= 4000);
    }

    SECTION("never once it is closed") {
        setup = [](const sse_ptr& sse) {
            sse->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .low_watermark = 512 << 10});
            for (int i = 0; i < 200; ++i) sse->send_data(std::string(64 * 1024, 'y'));
            sse->stop();
        };
        auto client = connect();
        REQUIRE(lows_in_handler.get_future().get() == 0);
        auto weak = created.get_future().get();
        client->read_events(SIZE_MAX, 5s);
        REQUIRE(client->closed());
        REQUIRE(eventually([&] { return weak.expired(); }));
        REQUIRE(lows->load() == 0);
    }
}

TEST_CASE("SSE write timeout", "[sse][flow][integration]") {
    constexpr size_t size = 64 * 1024;
    http::server server;
    auto state = std::make_shared<producer_state>();
    server.get("/events", [state](http::request&, http::response& res) {
        res.start_sse([state](sse_ptr sse) -> awaitable<void> {
            sse->set_write_limits({.max_messages = 0, .high_watermark = 1 << 20, .write_timeout = 300ms});
            co_spawn(co_await boost::asio::this_coro::executor, producer(sse, state, 400, size), detached);
        });
    });
    running_server running(server);
    sse_client client(running.port);

    // The client does not read: the write in progress times out, the producer is told, and
    // the connection closed
    REQUIRE(eventually([&] { return state->done.load(); }));
    REQUIRE(state->failed.load());
    client.read_events(SIZE_MAX, 5s);
    REQUIRE(client.closed());
}

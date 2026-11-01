#include <catch2/catch_test_macros.hpp>
#include <thinger/asio/io_context_of.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/sse_connection.hpp>
#include "server_test_helpers.hpp"
#include <boost/asio/use_future.hpp>
#include <chrono>
#include <functional>
#include <future>
#include <optional>
#include <string>
#include <thread>
#include <vector>

// Framing of the SSE fields (values with line breaks) and the keep-alive comments, read back
// by a parser following the WHATWG event stream interpretation

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    // An event as an EventSource dispatches it
    struct sse_event {
        std::string type = "message";
        std::string data;
        std::string id;
    };

    // A raw client of an SSE stream over any transport: the request, then the stream parsed as
    // a browser does
    struct sse_client {
        http::test::raw_connection connection;
        std::string buffer;
        std::vector<std::string> comments;

        explicit sse_client(const http::test::transport_endpoint& to, const std::string& path = "/events")
            : connection(to) {
            std::string request = "GET " + path + " HTTP/1.1\r\nHost: localhost\r\n"
                                  "Accept: text/event-stream\r\n\r\n";
            REQUIRE(connection.write(request));
            auto head = read_until("\r\n\r\n", 5s);
            REQUIRE(head);
            REQUIRE(head->find("HTTP/1.1 200") != std::string::npos);
        }

        // Up to and including `delimiter`, or nothing if it does not arrive in time
        std::optional<std::string> read_until(const std::string& delimiter, std::chrono::milliseconds timeout) {
            if (!connection.read_until(buffer, delimiter, timeout)) return std::nullopt;
            auto end = buffer.find(delimiter) + delimiter.size();
            std::string data = buffer.substr(0, end);
            buffer.erase(0, end);
            return data;
        }

        // One line, its terminator (CRLF, CR or LF) removed; the server ends lines with LF
        std::optional<std::string> read_line(std::chrono::milliseconds timeout) {
            auto line = read_until("\n", timeout);
            if (!line) return std::nullopt;
            line->pop_back();
            if (!line->empty() && line->back() == '\r') line->pop_back();
            return line;
        }

        // The next event dispatched (WHATWG HTML, 9.2.6: comments ignored, fields processed,
        // a blank line dispatching the event if its data buffer is not empty)
        std::optional<sse_event> next_event(std::chrono::milliseconds timeout = 5000ms) {
            sse_event event;
            bool has_data = false;
            for (;;) {
                auto line = read_line(timeout);
                if (!line) return std::nullopt;
                if (line->empty()) {
                    if (!has_data) {
                        event = sse_event{};
                        continue;
                    }
                    if (!event.data.empty() && event.data.back() == '\n') event.data.pop_back();
                    return event;
                }
                if ((*line)[0] == ':') {
                    comments.push_back(line->substr(1));
                    continue;
                }
                auto colon = line->find(':');
                std::string name = line->substr(0, colon);
                std::string value = colon == std::string::npos ? std::string{} : line->substr(colon + 1);
                if (!value.empty() && value[0] == ' ') value.erase(0, 1);
                if (name == "data") {
                    event.data += value + "\n";
                    has_data = true;
                } else if (name == "event") {
                    event.type = value;
                } else if (name == "id") {
                    event.id = value;
                }
            }
        }
    };

    // A server publishing the connection of its stream; `on_accept` runs in the handler that
    // accepts it, where on_close() is set
    struct sse_server {
        using accept_hook = std::function<void(const std::shared_ptr<http::sse_connection>&)>;

        http::server server;
        accept_hook on_accept;
        std::promise<std::shared_ptr<http::sse_connection>> connected;
        std::optional<server_test::running_server> running;

        explicit sse_server(http::test::transport kind, accept_hook hook = {}) : on_accept(std::move(hook)) {
            server.get("/events", [this](http::request&, http::response& res) {
                res.start_sse([this](std::shared_ptr<http::sse_connection> connection) {
                    if (on_accept) on_accept(connection);
                    connected.set_value(std::move(connection));
                });
            });
            running.emplace(server, kind);
        }

        std::shared_ptr<http::sse_connection> accept() {
            auto future = connected.get_future();
            REQUIRE(future.wait_for(5s) == std::future_status::ready);
            return future.get();
        }

        const http::test::transport_endpoint& endpoint() const { return running->endpoint(); }
    };

}

TEST_CASE("SSE data with line breaks is sent as several data fields", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    sse_server server(kind);
    sse_client client(server.endpoint());
    auto sse = server.accept();

    SECTION("LF, CRLF and CR all end a line") {
        sse->send_data("line1\nline2\r\nline3\rline4");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->type == "message");
        CHECK(event->data == "line1\nline2\nline3\nline4");
    }

    SECTION("the bytes on the wire are one data field per line") {
        sse->send_data("a\r\nb");
        auto raw = client.read_until("\n\n", 5s);
        REQUIRE(raw);
        CHECK(*raw == "data: a\ndata: b\n\n");
    }

    SECTION("empty lines, and a trailing line break, are kept") {
        sse->send_data("first\n\nthird\n");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->data == "first\n\nthird\n");
    }

    SECTION("a value cannot inject fields or events") {
        sse->send_data("x\nevent: admin\ndata: injected\n\ndata: second event");
        sse->send_data("next");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->type == "message");
        CHECK(event->data == "x\nevent: admin\ndata: injected\n\ndata: second event");
        auto next = client.next_event();
        REQUIRE(next);
        CHECK(next->type == "message");
        CHECK(next->data == "next");
    }

    SECTION("pretty-printed JSON arrives whole") {
        std::string json = "{\n  \"a\": 1,\n  \"b\": [\n    2\n  ]\n}";
        REQUIRE(sse->send_event("update"));
        REQUIRE(sse->send_id("42"));
        sse->send_data(json);
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->type == "update");
        CHECK(event->id == "42");
        CHECK(event->data == json);
    }

    sse->stop();
}

TEST_CASE("SSE event names and ids with line breaks are refused", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    sse_server server(kind);
    sse_client client(server.endpoint());
    auto sse = server.accept();

    CHECK_FALSE(sse->send_event("a\nevent: admin"));
    CHECK_FALSE(sse->send_event("a\rb"));
    CHECK_FALSE(sse->send_event("a\r\nb"));
    CHECK_FALSE(sse->send_id("1\ndata: injected"));
    CHECK_FALSE(sse->send_id("1\r"));
    CHECK_FALSE(sse->send_id(std::string("1\0" "2", 3)));
    CHECK_FALSE(sse->send_event("x\ny"));
    CHECK_FALSE(sse->send_comment("a\ndata: injected"));
    CHECK_FALSE(sse->send_comment("a\rb"));
    // a whole event is refused with any of its fields, and nothing of it is sent
    CHECK_FALSE(sse->send(http::sse_event{.event = "a\nevent: admin", .data = "x"}));
    CHECK_FALSE(sse->send(http::sse_event{.id = "1\r", .data = "x"}));
    CHECK_FALSE(sse->send(http::sse_event{.id = std::string("1\0" "2", 3), .data = "x"}));
    CHECK_FALSE(sse->send(http::sse_event{.event = "ok", .retry = -1ms, .data = "x"}));
    auto send_async = [&](http::sse_event event) {
        return boost::asio::co_spawn(sse->get_executor(), sse->send_async(std::move(event)), boost::asio::use_future).get();
    };
    CHECK_FALSE(send_async(http::sse_event{.event = "a\r\nb", .data = "x"}));
    CHECK_FALSE(send_async(http::sse_event{.id = "1\ndata: injected", .data = "x"}));

    // nothing of the refused fields was sent: the next event is the first one valid
    REQUIRE(sse->send_event("ok"));
    REQUIRE(sse->send_id("7"));
    sse->send_data("payload");
    auto raw = client.read_until("\n\n", 5s);
    REQUIRE(raw);
    CHECK(*raw == "event: ok\nid: 7\ndata: payload\n\n");

    sse->stop();
}

TEST_CASE("SSE sends keep-alive comments on a quiet stream", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    sse_server server(kind);
    sse_client client(server.endpoint());
    auto sse = server.accept();

    SECTION("at the interval set, and the stream stays open") {
        sse->set_keepalive(200ms);
        for (int i = 0; i < 3; ++i) {
            auto line = client.read_until("\n", 3s);
            REQUIRE(line);
            CHECK(*line == ": keep-alive\n");
        }
        // events still go through, and are parsed around the comments
        sse->send_data("still here");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->data == "still here");
    }

    SECTION("in the middle of an event, without dispatching it") {
        sse->set_keepalive(150ms);
        REQUIRE(sse->send_event("late"));
        // the event field, then comments while its data does not come
        std::this_thread::sleep_for(600ms);
        sse->send_data("value");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->type == "late");
        CHECK(event->data == "value");
        REQUIRE_FALSE(client.comments.empty());
        CHECK(client.comments.front() == " keep-alive");
    }

    SECTION("not while events are sent") {
        sse->set_keepalive(300ms);
        auto until = std::chrono::steady_clock::now() + 900ms;
        while (std::chrono::steady_clock::now() < until) {
            sse->send_data("tick");
            auto event = client.next_event(2s);
            REQUIRE(event);
            CHECK(event->data == "tick");
            std::this_thread::sleep_for(50ms);
        }
        CHECK(client.comments.empty());
    }

    SECTION("disabled with 0") {
        sse->set_keepalive(0ms);
        CHECK_FALSE(client.read_until("\n", 700ms));
    }

    sse->stop();
}

TEST_CASE("SSE stream closes when the client leaves, without a keep-alive", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    // (shared: the connection may outlive the test if it is not told)
    auto closed = std::make_shared<std::promise<void>>();
    auto future = closed->get_future();
    sse_server server(kind, [closed](const std::shared_ptr<http::sse_connection>& sse) {
        sse->set_keepalive(0ms);
        sse->on_close([closed] { closed->set_value(); });
    });
    std::optional<sse_client> client(std::in_place, server.endpoint());
    auto sse = server.accept();
    client.reset();
    CHECK(future.wait_for(5s) == std::future_status::ready);
}

TEST_CASE("SSE events sent whole are on the wire as their fields sent one by one", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    sse_server server(kind);
    sse_client client(server.endpoint());
    auto sse = server.accept();

    SECTION("every field, and data of several lines") {
        REQUIRE(sse->send_event("update"));
        REQUIRE(sse->send_id("42"));
        sse->send_retry(3000);
        sse->send_data("a\r\nb\nc");
        auto fields = client.read_until("\n\n", 5s);
        REQUIRE(sse->send(http::sse_event{.event = "update", .id = "42", .retry = 3000ms, .data = "a\r\nb\nc"}));
        auto whole = client.read_until("\n\n", 5s);
        REQUIRE(fields);
        REQUIRE(whole);
        CHECK(*fields == "event: update\nid: 42\nretry: 3000\ndata: a\ndata: b\ndata: c\n\n");
        CHECK(*whole == *fields);
    }

    SECTION("data alone, also awaited") {
        sse->send_data("x");
        auto fields = client.read_until("\n\n", 5s);
        REQUIRE(sse->send(http::sse_event{.data = "x"}));
        auto whole = client.read_until("\n\n", 5s);
        auto written = boost::asio::co_spawn(sse->get_executor(), sse->send_async(http::sse_event{.data = "x"}),
                                             boost::asio::use_future);
        auto awaited = client.read_until("\n\n", 5s);
        REQUIRE(written.wait_for(5s) == std::future_status::ready);
        CHECK(written.get());
        REQUIRE(fields);
        CHECK(*fields == "data: x\n\n");
        CHECK(whole == fields);
        CHECK(awaited == fields);
    }

    sse->stop();
}

TEST_CASE("SSE execute() runs on the executor of the connection", "[server][sse][executor][integration]") {
    sse_server server(http::test::transport::tcp);
    sse_client client(server.endpoint());
    auto sse = server.accept();
    REQUIRE_FALSE(asio::running_in_this_thread(sse->get_executor()));

    SECTION("from another thread: there") {
        std::promise<bool> ran;
        sse->execute([&] { ran.set_value(asio::running_in_this_thread(sse->get_executor())); });
        auto on_executor = ran.get_future();
        REQUIRE(on_executor.wait_for(5s) == std::future_status::ready);
        CHECK(on_executor.get());
    }

    SECTION("from its executor: at once") {
        std::promise<bool> inline_run;
        sse->execute([&] {
            bool ran = false;
            sse->execute([&] { ran = true; });
            inline_run.set_value(ran);
        });
        auto at_once = inline_run.get_future();
        REQUIRE(at_once.wait_for(5s) == std::future_status::ready);
        CHECK(at_once.get());
    }

    sse->stop();
}

TEST_CASE("SSE keep-alive comments every 100 ms", "[server][sse][integration][matrix]") {
    MATRIX_TRANSPORT(kind);
    sse_server server(kind);
    sse_client client(server.endpoint());
    auto sse = server.accept();

    SECTION("on a quiet stream, one per interval") {
        auto set = std::chrono::steady_clock::now();
        sse->set_keepalive(100ms);
        for (int i = 0; i < 3; ++i) {
            auto line = client.read_until("\n", 3s);
            REQUIRE(line);
            CHECK(*line == ": keep-alive\n");
        }
        // the third one once three intervals passed
        CHECK(std::chrono::steady_clock::now() - set >= 280ms);
    }

    SECTION("none with continuous traffic") {
        sse->set_keepalive(100ms);
        for (int i = 0; i < 60; ++i) {
            sse->send_data("tick");
            std::this_thread::sleep_for(10ms);
        }
        for (int i = 0; i < 60; ++i) {
            auto event = client.next_event(2s);
            REQUIRE(event);
            CHECK(event->data == "tick");
        }
        CHECK(client.comments.empty());
    }

    SECTION("0 disables them") {
        sse->set_keepalive(100ms);
        auto line = client.read_until("\n", 3s);
        REQUIRE(line);
        CHECK(*line == ": keep-alive\n");
        sse->set_keepalive(0ms);
        // sent after it is disabled: no comment after it
        sse->send_data("marker");
        auto event = client.next_event();
        REQUIRE(event);
        CHECK(event->data == "marker");
        CHECK_FALSE(client.read_until("\n", 500ms));
    }

    sse->stop();
}

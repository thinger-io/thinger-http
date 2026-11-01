#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/http_stream.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/request_factory.hpp>
#include <thinger/http/common/http_request.hpp>
#include <thinger/http/common/http_response.hpp>
#include <thinger/http/common/http_data.hpp>
#include <thinger/http/data/out_string.hpp>
#include <boost/asio/io_context.hpp>
#include <memory>
#include <string>
#include <thread>

// What happens to a connection after the response of a stream (its disposition): seeded from
// the request, closed for the first reason given and never kept alive again, taken over once a
// takeover is set. And what each frame the server queues is

using namespace thinger::http;
using detail::after_response;
using detail::close_reason;

namespace {

    // The request of `head` (the request line and headers, without the blank line), as the
    // server parses it
    std::shared_ptr<http_request> parse(const std::string& head) {
        request_factory parser;
        parser.set_headers_only(true);
        std::string raw = head + "\r\n";
        auto* begin = reinterpret_cast<const uint8_t*>(raw.data());
        auto* end = begin + raw.size();
        auto* it = begin;
        boost::tribool result = parser.parse(it, end);
        REQUIRE(bool(result));
        return parser.consume_request();
    }

    // The executor of the streams (nothing runs on it)
    boost::asio::io_context io;

    http_stream stream_of(const std::string& head) {
        return http_stream(1, *parse(head), io.get_executor());
    }

}

TEST_CASE("A stream is seeded from its request", "[server][disposition][unit]") {

    SECTION("HTTP/1.1: kept alive") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\n");
        REQUIRE(stream.disposition() == after_response::keep_alive);
        REQUIRE_FALSE(stream.reason());
    }

    SECTION("HTTP/1.1 with Connection: keep-alive: kept alive") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: keep-alive\r\n");
        REQUIRE(stream.disposition() == after_response::keep_alive);
    }

    SECTION("HTTP/1.1 with Connection: close: the client asked") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n");
        REQUIRE(stream.disposition() == after_response::close);
        REQUIRE(stream.reason() == close_reason::client_asked);
    }

    SECTION("close among other options, in any case") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: TE, Close\r\n");
        REQUIRE(stream.reason() == close_reason::client_asked);
    }

    SECTION("HTTP/1.0: closed") {
        auto stream = stream_of("GET / HTTP/1.0\r\n");
        REQUIRE(stream.disposition() == after_response::close);
        REQUIRE(stream.reason() == close_reason::http10);
    }

    SECTION("HTTP/1.0 with Connection: keep-alive: kept alive") {
        auto stream = stream_of("GET / HTTP/1.0\r\nConnection: keep-alive\r\n");
        REQUIRE(stream.disposition() == after_response::keep_alive);
    }

    SECTION("HTTP/1.0 with Connection: close: an HTTP/1.0 client") {
        auto stream = stream_of("GET / HTTP/1.0\r\nConnection: close\r\n");
        REQUIRE(stream.reason() == close_reason::http10);
    }

    SECTION("A stream created for a reason closes for it") {
        http_stream stream(1, close_reason::body_error, io.get_executor());
        REQUIRE(stream.disposition() == after_response::close);
        REQUIRE(stream.reason() == close_reason::body_error);
    }
}

TEST_CASE("A stream never goes back to keep-alive", "[server][disposition][unit]") {
    auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\n");

    SECTION("The first reason stays, whatever comes after it") {
        stream.must_close(close_reason::draining);
        REQUIRE(stream.disposition() == after_response::close);
        REQUIRE(stream.reason() == close_reason::draining);

        for (auto reason : {close_reason::client_asked, close_reason::http10, close_reason::draining,
                            close_reason::continue_skipped, close_reason::body_unread, close_reason::body_error,
                            close_reason::handler_failed, close_reason::route_timeout,
                            close_reason::request_timeout, close_reason::framing_by_close, close_reason::aborted}) {
            stream.must_close(reason);
            REQUIRE(stream.disposition() == after_response::close);
            REQUIRE(stream.reason() == close_reason::draining);
        }
    }

    SECTION("A seeded reason stays too") {
        auto closing = stream_of("GET / HTTP/1.0\r\n");
        closing.must_close(close_reason::aborted);
        REQUIRE(closing.reason() == close_reason::http10);
    }

    SECTION("Nothing else changes it: frames, continue state, input pending") {
        stream.must_close(close_reason::body_unread);
        stream.add_frame(std::make_shared<http_response>());
        stream.set_continue(http_stream::continue_state::skipped);
        stream.set_input_pending(false);
        stream.pop_frame();
        REQUIRE(stream.disposition() == after_response::close);
        REQUIRE(stream.reason() == close_reason::body_unread);
    }
}

TEST_CASE("A takeover is not turned into a close", "[server][disposition][takeover][unit]") {

    SECTION("Set on a stream kept alive: take_over, also after a reason") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\n");
        stream.set_takeover([](upgraded_connection) {});
        REQUIRE(stream.disposition() == after_response::take_over);
        REQUIRE(stream.takes_over());
        REQUIRE(stream.responded());

        // The reason only makes its head say Connection: close
        stream.must_close(close_reason::framing_by_close);
        REQUIRE(stream.disposition() == after_response::take_over);
        REQUIRE(stream.reason() == close_reason::framing_by_close);
    }

    SECTION("Set on a stream already closing: take_over, the reason kept") {
        auto stream = stream_of("GET / HTTP/1.1\r\nHost: localhost\r\nConnection: Upgrade, close\r\n");
        REQUIRE(stream.disposition() == after_response::close);
        stream.set_takeover([](upgraded_connection) {});
        REQUIRE(stream.disposition() == after_response::take_over);
        REQUIRE(stream.reason() == close_reason::client_asked);
    }
}

TEST_CASE("Whether input of the request is pending is its own fact", "[server][disposition][linger][unit]") {
    auto stream = stream_of("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 10\r\n");
    REQUIRE_FALSE(stream.input_pending());

    SECTION("Pending with a close: the connection lingers") {
        stream.set_input_pending(true);
        stream.must_close(close_reason::body_unread);
        REQUIRE(stream.input_pending());
        REQUIRE(stream.disposition() == after_response::close);
    }

    SECTION("Read to its end: closed at once") {
        stream.set_input_pending(true);
        stream.set_input_pending(false);
        stream.must_close(close_reason::handler_failed);
        REQUIRE_FALSE(stream.input_pending());
        REQUIRE(stream.disposition() == after_response::close);
    }

    SECTION("Pending input does not close a stream kept alive") {
        stream.set_input_pending(true);
        REQUIRE(stream.disposition() == after_response::keep_alive);
    }
}

TEST_CASE("Frames tell their kind", "[server][frame][unit]") {

    SECTION("Heads: a response, a request") {
        http_response response;
        http_request request;
        REQUIRE(response.kind() == frame_kind::head);
        REQUIRE(request.kind() == frame_kind::head);
        REQUIRE(http_response::stock_http_reply(http_response::status::bad_request)->kind() == frame_kind::head);
    }

    SECTION("100 Continue: interim") {
        detail::continue_frame frame;
        REQUIRE(frame.kind() == frame_kind::interim);
        REQUIRE_FALSE(frame.end_stream());
    }

    SECTION("Body data: data") {
        http_data empty;
        http_data body(std::make_shared<thinger::http::data::out_string>("chunk"));
        REQUIRE(empty.kind() == frame_kind::data);
        REQUIRE(body.kind() == frame_kind::data);
    }

    SECTION("Through the base class, as the connection asks") {
        std::shared_ptr<http_frame> frames[] = {std::make_shared<http_response>(),
                                                std::make_shared<detail::continue_frame>(),
                                                std::make_shared<http_data>()};
        REQUIRE(frames[0]->kind() == frame_kind::head);
        REQUIRE(frames[1]->kind() == frame_kind::interim);
        REQUIRE(frames[2]->kind() == frame_kind::data);
    }
}

TEST_CASE("The cancellation scope of a stream", "[server][cancellation][unit]") {
    // On its own io_context, run by the test (cancel() acts there)
    boost::asio::io_context context;
    auto stream = std::make_shared<http_stream>(1, *parse("GET / HTTP/1.1\r\nHost: localhost\r\n"),
                                                context.get_executor());
    int handler_emitted = 0;
    int observer_emitted = 0;
    stream->handler_slot().assign([&](boost::asio::cancellation_type) { ++handler_emitted; });
    stream->observer_slot().assign([&](boost::asio::cancellation_type) { ++observer_emitted; });

    SECTION("none until cancelled; none is not a reason") {
        REQUIRE(stream->cancelled() == cancel_reason::none);
        stream->cancel(cancel_reason::none);
        context.poll();
        REQUIRE(stream->cancelled() == cancel_reason::none);
    }

    SECTION("The first reason stays, and the scope is cancelled once") {
        stream->handler_started();
        stream->cancel(cancel_reason::timeout);
        stream->cancel(cancel_reason::client_gone);
        context.poll();
        REQUIRE(stream->cancelled() == cancel_reason::timeout);
        REQUIRE(observer_emitted == 1);
    }

    SECTION("A reason the route did not declare reaches only the observer") {
        stream->set_cancel_on(1u << static_cast<unsigned>(cancel_reason::client_gone));
        stream->handler_started();
        stream->cancel(cancel_reason::timeout);
        context.poll();
        REQUIRE(observer_emitted == 1);
        REQUIRE(handler_emitted == 0);
        REQUIRE_FALSE(stream->handler_interrupted());
    }

    SECTION("A declared reason interrupts the handler") {
        stream->set_cancel_on(1u << static_cast<unsigned>(cancel_reason::client_gone));
        stream->handler_started();
        stream->cancel(cancel_reason::client_gone);
        context.poll();
        REQUIRE(handler_emitted == 1);
        REQUIRE(stream->handler_interrupted());
    }

    SECTION("Shutdown interrupts every handler") {
        stream->handler_started();
        stream->cancel(cancel_reason::shutdown);
        context.poll();
        REQUIRE(handler_emitted == 1);
        REQUIRE(observer_emitted == 1);
    }

    SECTION("A handler bound to a signal of its owner is interrupted through it") {
        boost::asio::cancellation_signal owner;
        int owner_emitted = 0;
        owner.slot().assign([&](boost::asio::cancellation_type) { ++owner_emitted; });
        REQUIRE_FALSE(stream->handler_bound());
        stream->bind_handler(&owner);
        REQUIRE(stream->handler_bound());
        stream->handler_started();
        stream->cancel(cancel_reason::shutdown);
        context.poll();
        REQUIRE(owner_emitted == 1);
        REQUIRE(handler_emitted == 0);
        REQUIRE(stream->handler_interrupted());
    }

    SECTION("Before the handler starts or after it ends, nothing is emitted") {
        stream->handler_started();
        stream->handler_ended();
        stream->cancel(cancel_reason::shutdown);
        context.poll();
        REQUIRE(stream->cancelled() == cancel_reason::shutdown);
        REQUIRE(handler_emitted == 0);
        REQUIRE(observer_emitted == 0);
    }

    SECTION("The wait ends with the reason, or none if it is cancelled itself") {
        std::optional<cancel_reason> reason;
        co_spawn(context, stream->wait_cancelled(), [&](std::exception_ptr, cancel_reason r) { reason = r; });
        context.poll();
        REQUIRE_FALSE(reason);
        stream->cancel(cancel_reason::client_gone);
        context.poll();
        REQUIRE(reason == cancel_reason::client_gone);

        // (the io_context stopped once out of work)
        context.restart();
        auto other = std::make_shared<http_stream>(2, close_reason::body_error, context.get_executor());
        boost::asio::cancellation_signal signal;
        reason.reset();
        co_spawn(context, other->wait_cancelled(),
                 boost::asio::bind_cancellation_slot(signal.slot(),
                                                     [&](std::exception_ptr, cancel_reason r) { reason = r; }));
        context.poll();
        signal.emit(boost::asio::cancellation_type::terminal);
        context.poll();
        REQUIRE(reason == cancel_reason::none);
    }

    SECTION("A first wait after a cancel from another thread ends with its reason") {
        // The cancel is given on another thread: its half on the executor of the stream has
        // not run when the wait starts, nor after (each in its order)
        auto cancel_elsewhere = [&](cancel_reason first) {
            std::thread([&] {
                stream->cancel(first);
                stream->cancel(cancel_reason::shutdown);
            }).join();
        };
        std::optional<cancel_reason> before;
        std::optional<cancel_reason> after;

        SECTION("its half on the executor still to run") {
            // (posted there, it runs once this coroutine yields)
            co_spawn(context, [&]() -> thinger::awaitable<void> {
                cancel_elsewhere(cancel_reason::client_gone);
                before = co_await stream->wait_cancelled();
            }, boost::asio::detached);
            context.poll();
        }
        SECTION("its half on the executor run already") {
            cancel_elsewhere(cancel_reason::client_gone);
            context.poll();
            context.restart();
            co_spawn(context, stream->wait_cancelled(), [&](std::exception_ptr, cancel_reason r) { before = r; });
            context.poll();
        }
        REQUIRE(before == cancel_reason::client_gone);
        // A later wait too, and the first reason stayed
        context.restart();
        co_spawn(context, stream->wait_cancelled(), [&](std::exception_ptr, cancel_reason r) { after = r; });
        context.poll();
        REQUIRE(after == cancel_reason::client_gone);
        REQUIRE(stream->cancelled() == cancel_reason::client_gone);
    }

    SECTION("A wait started before a cancel from another thread ends with its reason") {
        std::optional<cancel_reason> reason;
        co_spawn(context, stream->wait_cancelled(), [&](std::exception_ptr, cancel_reason r) { reason = r; });
        context.poll();
        REQUIRE_FALSE(reason);
        std::thread([&] { stream->cancel(cancel_reason::timeout); }).join();
        context.restart();
        context.run();
        REQUIRE(reason == cancel_reason::timeout);
    }

    stream->handler_slot().clear();
    stream->observer_slot().clear();
}

TEST_CASE("The wait for the input of a request ends once it is read", "[server][linger][unit]") {
    boost::asio::io_context context;
    auto stream = std::make_shared<http_stream>(1, *parse("POST / HTTP/1.1\r\nHost: localhost\r\nContent-Length: 4\r\n"),
                                                context.get_executor());
    std::optional<bool> read;

    SECTION("started while it is pending") {
        stream->set_input_pending(true);
        co_spawn(context, stream->wait_input_read(), [&](std::exception_ptr, bool r) { read = r; });
        context.poll();
        REQUIRE_FALSE(read);
        stream->set_input_pending(false, true);
        context.restart();
        context.poll();
        REQUIRE(read == true);
        REQUIRE(stream->input_follows());
    }

    SECTION("started once it was read: at once") {
        stream->set_input_pending(true);
        stream->set_input_pending(false);
        co_spawn(context, stream->wait_input_read(), [&](std::exception_ptr, bool r) { read = r; });
        context.poll();
        REQUIRE(read == true);
    }

    SECTION("started on a stream with no input: at once") {
        co_spawn(context, stream->wait_input_read(), [&](std::exception_ptr, bool r) { read = r; });
        context.poll();
        REQUIRE(read == true);
    }
}

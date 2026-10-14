// The response of the target, relayed by the reverse proxy to the client
#include "proxy_exchange.hpp"
#include "../../common/chunked_decoder.hpp"
#include <algorithm>

namespace thinger::http::proxy_detail {

namespace {

    // Interim responses (1xx) skipped before giving up on a target
    constexpr unsigned max_interim_responses = 16;

    // Whether a read from the target ended at the end of its stream (not an error). A TLS
    // target closing without close_notify (stream_truncated) does not end a body delimited
    // by the connection: it may have been cut, so it is relayed as incomplete
    bool is_end_of_stream(const boost::system::error_code& ec) {
        return ec == boost::asio::error::eof;
    }

}

awaitable<bool> proxy_exchange::send_to_client(std::string data) {
    auto size = data.size();
    if (!co_await res_.write_stream(std::move(data))) {
        result_.error = "client connection lost";
        co_return false;
    }
    result_.response_bytes += size;
    touch();
    co_return true;
}

awaitable<bool> proxy_exchange::read_head() {
    std::vector<uint8_t> buffer(buffer_size);
    for (unsigned interim = 0; ; ++interim) {
        // The head alone: the body is relayed as it is read. Its framing depends on the
        // request the target got (the head for the client, on the request of the client:
        // see response::start_stream())
        response_factory parser;
        parser.set_headers_only(true);
        parser.set_request_method(outgoing_method_);

        while (true) {
            if (pending_.empty()) {
                auto [ec, bytes] = co_await read_target(buffer.data(), buffer.size());
                if (ec || bytes == 0) {
                    fail(http_response::status::bad_gateway, "Invalid response from the target",
                         "the target closed the connection without a response" +
                         (ec ? ": " + ec.message() : std::string{}));
                    co_return false;
                }
                pending_.assign(reinterpret_cast<const char*>(buffer.data()), bytes);
            }

            const char* begin = pending_.data();
            const char* end = begin + pending_.size();
            boost::tribool parsed = parser.parse_some(begin, end);
            pending_.erase(0, static_cast<size_t>(begin - pending_.data()));
            if (parsed) break;
            if (!parsed) {
                fail(http_response::status::bad_gateway, "Invalid response from the target",
                     make_error_code(parser.error()).message());
                co_return false;
            }
        }

        auto head = parser.consume_response();
        int code = head->get_status_code();

        // Interim responses (100 Continue, 103 Early Hints...) are not relayed
        if (code < 200 && code != 101) {
            if (interim + 1 >= max_interim_responses) {
                fail(http_response::status::bad_gateway, "Invalid response from the target",
                     "too many interim responses");
                co_return false;
            }
            continue;
        }

        head_ = std::move(head);
        status_code_ = code;
        body_framing_ = parser.get_body_framing();
        co_return true;
    }
}

std::optional<size_t> proxy_exchange::content_length() const {
    // The framing of the target is unambiguous (the parser rejected anything else): a valid
    // Content-Length, or "Transfer-Encoding: chunked" alone, or neither (the body ends with
    // the connection)
    if (head_->is_chunked_transfer() || !head_->has_content_length_header()) return std::nullopt;
    return head_->get_content_length();
}

std::optional<size_t> proxy_exchange::advertised_length() const {
    // Without a body, the length of the resource is only advertised (HEAD, 304)
    bool no_body = body_framing_ == response_factory::body_framing::none;
    bool advertises = outgoing_method_ == method::HEAD || incoming_->get_method() == method::HEAD ||
                      status_code_ == 304;
    if (no_body && !advertises) return std::nullopt;
    return content_length();
}

awaitable<bool> proxy_exchange::relay_length(size_t length) {
    while (length > 0) {
        std::string data;
        if (!pending_.empty()) {
            auto size = std::min(length, pending_.size());
            data = pending_.substr(0, size);
            pending_.erase(0, size);
        } else {
            data.resize(std::min(length, buffer_size));
            auto [ec, bytes] = co_await read_target(reinterpret_cast<uint8_t*>(data.data()), data.size());
            if (ec || bytes == 0) {
                fail(http_response::status::bad_gateway, "", "response body truncated by the target");
                co_return false;
            }
            data.resize(bytes);
        }
        length -= data.size();
        if (!co_await send_to_client(std::move(data))) co_return false;
    }
    co_return true;
}

awaitable<bool> proxy_exchange::relay_chunked() {
    // Decoded strictly (the decoder of the request bodies), and chunked again (or not) for
    // the client: chunk extensions and trailers are dropped
    chunked_decoder decoder;
    std::vector<uint8_t> buffer(buffer_size);
    while (!decoder.done()) {
        if (pending_.empty()) {
            auto [ec, bytes] = co_await read_target(buffer.data(), buffer.size());
            if (ec || bytes == 0) {
                fail(http_response::status::bad_gateway, "", "chunked response body truncated by the target");
                co_return false;
            }
            pending_.assign(reinterpret_cast<const char*>(buffer.data()), bytes);
        }
        std::string data(buffer_size, '\0');
        size_t produced = 0;
        auto consumed = decoder.decode(reinterpret_cast<const uint8_t*>(pending_.data()), pending_.size(),
                                       reinterpret_cast<uint8_t*>(data.data()), data.size(), produced);
        pending_.erase(0, consumed);
        if (decoder.failed()) {
            fail(http_response::status::bad_gateway, "", "malformed chunked response body");
            co_return false;
        }
        if (produced == 0) continue;
        data.resize(produced);
        if (!co_await send_to_client(std::move(data))) co_return false;
    }
    co_return true;
}

awaitable<bool> proxy_exchange::relay_until_close() {
    if (!pending_.empty() && !co_await send_to_client(std::exchange(pending_, {}))) co_return false;
    while (true) {
        std::string data(buffer_size, '\0');
        auto [ec, bytes] = co_await read_target(reinterpret_cast<uint8_t*>(data.data()), data.size());
        if (is_end_of_stream(ec)) co_return true;
        if (ec) {
            fail(http_response::status::bad_gateway, "", "response body interrupted: " + ec.message());
            co_return false;
        }
        data.resize(bytes);
        if (!co_await send_to_client(std::move(data))) co_return false;
    }
}

awaitable<bool> proxy_exchange::relay_body() {
    switch (body_framing_) {
        case response_factory::body_framing::chunked:
            co_return co_await relay_chunked();
        case response_factory::body_framing::content_length:
            co_return co_await relay_length(*content_length());
        case response_factory::body_framing::until_close:
            co_return co_await relay_until_close();
        case response_factory::body_framing::none:
            break;
    }
    co_return true;
}

awaitable<void> proxy_exchange::relay_response() {
    // Head for the client: the status and end-to-end headers of the target
    http_response out;
    out.set_status(static_cast<uint16_t>(status_code_));
    out.set_reason_phrase(head_->get_reason());
    copy_end_to_end(*head_, out);
    if (options_.on_response) options_.on_response(req_, out);
    if (!has_valid_headers(out)) {
        fail(http_response::status::bad_gateway, "Invalid response from the target", "invalid response header");
        co_return;
    }

    if (!res_.start_stream(out, advertised_length())) {
        result_.error = "the response could not be sent";
        co_return;
    }
    response_started_ = true;
    result_.status = out.get_status_code();
    result_.from_target = true;

    if (!co_await relay_body()) {
        // the client sees the response incomplete (no last chunk, or connection closed)
        res_.abort_stream();
        co_return;
    }
    response_ended_ = true;
    if (!co_await res_.end_stream()) {
        result_.error = "client connection lost";
        co_return;
    }
    result_.complete = request_sent_;
}

}

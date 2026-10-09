#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/response_sink.hpp>
#include <nlohmann/json.hpp>
#include <atomic>
#include <functional>
#include <latch>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace thinger::http;

namespace {

    // Sink recording what a response writes, from any thread
    class recording_sink : public response_sink {
    public:
        void send(std::shared_ptr<http_response> response) override {
            std::lock_guard lock(mutex_);
            events_.push_back("send");
            response_ = std::move(response);
        }

        bool begin(std::shared_ptr<http_response> headers) override {
            std::lock_guard lock(mutex_);
            events_.push_back("begin");
            response_ = std::move(headers);
            return true;
        }

        bool append(const std::string& data) override {
            std::lock_guard lock(mutex_);
            events_.push_back("chunk:" + data);
            return true;
        }

        bool finish() override {
            std::lock_guard lock(mutex_);
            events_.push_back("finish");
            return true;
        }

        std::vector<std::string> events() const {
            std::lock_guard lock(mutex_);
            return events_;
        }

        std::shared_ptr<http_response> response() const {
            std::lock_guard lock(mutex_);
            return response_;
        }

    private:
        mutable std::mutex mutex_;
        std::vector<std::string> events_;
        std::shared_ptr<http_response> response_;
    };

    std::shared_ptr<http_request> make_request() {
        auto request = std::make_shared<http_request>();
        request->set_method(method::GET);
        request->set_uri("/");
        // big enough bodies are compressed: compression runs concurrently too
        request->add_header("Accept-Encoding", "gzip");
        return request;
    }

    // Run every action at once, each on its own thread
    void run_concurrently(const std::vector<std::function<void()>>& actions) {
        std::latch start(static_cast<std::ptrdiff_t>(actions.size()));
        std::vector<std::thread> threads;
        for (const auto& action : actions) {
            threads.emplace_back([&start, &action] {
                start.arrive_and_wait();
                action();
            });
        }
        for (auto& thread : threads) thread.join();
    }

}

TEST_CASE("Concurrent answers from copies of a response send only the first one", "[server][response][unit]") {
    const std::string long_text(512, 'x');

    for (int i = 0; i < 300; ++i) {
        auto sink = std::make_shared<recording_sink>();
        response res(sink, make_request(), true);
        res.header("X-Common", "set before answering");

        response json_copy = res, error_copy = res, text_copy = res, redirect_copy = res, chunked_copy = res;
        run_concurrently({
            [&] { json_copy.json({{"from", "json"}, {"padding", long_text}}, http_response::status::created); },
            [&] { error_copy.error(http_response::status::service_unavailable, "from error " + long_text); },
            [&] { text_copy.send("from send " + long_text); },
            [&] { redirect_copy.redirect("/elsewhere"); },
            [&] {
                if (chunked_copy.start_chunked("text/plain")) {
                    chunked_copy.write_chunk("from chunked");
                    chunked_copy.end_chunked();
                }
            },
        });

        // A single answer was written, whole: never a mix of several
        auto events = sink->events();
        REQUIRE_FALSE(events.empty());
        auto message = sink->response();
        REQUIRE(message);
        REQUIRE(message->get_header("X-Common") == "set before answering");
        REQUIRE(message->get_header("Access-Control-Allow-Origin") == "*");
        std::set<std::string> keys;
        for (const auto& [key, value] : message->get_headers()) {
            REQUIRE(keys.insert(key).second);   // headers of a single answer, not repeated
        }

        if (events.front() == "begin") {
            REQUIRE(events == std::vector<std::string>{"begin", "chunk:from chunked", "finish"});
            REQUIRE(message->get_status() == http_response::status::ok);
            continue;
        }

        REQUIRE(events == std::vector<std::string>{"send"});
        switch (message->get_status()) {
            case http_response::status::created:
                REQUIRE(message->get_content_type() == "application/json");
                REQUIRE(message->get_header("Content-Encoding") == "gzip");
                break;
            case http_response::status::service_unavailable:
                REQUIRE(message->get_content_type() == "text/plain");
                REQUIRE(message->get_header("Content-Encoding") == "gzip");
                break;
            case http_response::status::moved_temporarily:
                REQUIRE(message->get_header("Location") == "/elsewhere");
                break;
            case http_response::status::ok:
                REQUIRE(message->get_content_type() == "text/plain");
                REQUIRE(message->get_header("Content-Encoding") == "gzip");
                break;
            default:
                FAIL("unexpected status " << message->get_status_code());
        }
    }
}

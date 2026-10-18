#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/response_sink.hpp>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

// Allocations made answering a request: the heap allocations of the calls of a handler, counted
// by replacing the global operator new in this test program (it is not part of the test runners
// gathering every test, nor built with the sanitizers, which replace it themselves)

namespace {

    thread_local bool counting = false;
    thread_local size_t allocations = 0;

    // Allocations made by `f` on this thread
    template<typename F>
    size_t allocations_of(F f) {
        allocations = 0;
        counting = true;
        f();
        counting = false;
        return allocations;
    }

    // Keeps the response sent, as a connection queues it
    class keeping_sink : public thinger::http::response_sink {
    public:
        void send(std::shared_ptr<thinger::http::http_response> response) override { sent = std::move(response); }
        bool begin(std::shared_ptr<thinger::http::http_response>) override { return false; }
        bool append(const std::string&) override { return false; }
        bool finish() override { return false; }

        std::shared_ptr<thinger::http::http_response> sent;
    };

}

void* operator new(std::size_t size) {
    if (counting) ++allocations;
    if (void* memory = std::malloc(size ? size : 1)) return memory;
    throw std::bad_alloc();
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}

using namespace thinger::http;

TEST_CASE("Allocations of a response with a header set before answering", "[response][allocations][unit]") {
    auto request = http_request::create_http_request(method::GET, "http://localhost/hello");
    const std::string request_id = "3f2c9a1e-8b7d-4c6e-9f0a-1b2c3d4e5f60";

    auto answer = [&](auto set_headers) {
        auto sink = std::make_shared<keeping_sink>();
        auto count = allocations_of([&] {
            response res(sink, request);
            set_headers(res);
            res.send("hello");
        });
        REQUIRE(sink->sent);
        return count;
    };

    auto plain = answer([](response&) {});
    auto lvalue = answer([&](response& res) { res.header("X-Request-Id", request_id); });
    // a value made before (e.g. a generated id), moved in
    std::string id = request_id;
    auto moved = answer([&](response& res) { res.header("X-Request-Id", std::move(id)); });
    auto two = answer([&](response& res) {
        res.header("X-Request-Id", request_id);
        res.header("Cache-Control", "no-store");
    });

    INFO("allocations of a response: " << plain << ", with a header copied: " << lvalue
         << ", moved: " << moved << ", two headers: " << two);
    // The status and headers set before answering are moved into the answer, not copied: a
    // header costs the copy of its value (a long one, as it is passed as an lvalue), and
    // nothing when it is moved in. The headers have room for a few (http_response reserves it)
    CHECK(moved == plain);
    CHECK(lvalue == plain + 1);
    CHECK(two == plain + 1);
}

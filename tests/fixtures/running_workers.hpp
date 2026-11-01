#ifndef THINGER_HTTP_TEST_RUNNING_WORKERS_HPP
#define THINGER_HTTP_TEST_RUNNING_WORKERS_HPP

#include <thinger/asio/workers.hpp>
#include <thinger/http/client/async_client.hpp>
#include <cstddef>

namespace thinger::http::test {

// Workers started for a test (for its pool_servers and async_clients), stopped when it ends.
// Declare them before the servers and clients that run on them, so they outlive them
struct running_workers : asio::workers {
    explicit running_workers(size_t threads = 2) { start(threads); }
    ~running_workers() { if (running()) stop(); }
};

// Workers started before the client that runs on them (a base, so they outlive it)
struct with_workers {
    running_workers workers;
};

// A client of the type under test, default-constructed: an async_client runs on workers of
// its own
template<typename Client>
struct client_under_test : Client {};

template<>
struct client_under_test<async_client> : with_workers, async_client {
    client_under_test() : async_client(workers) {}
};

} // namespace thinger::http::test

#endif // THINGER_HTTP_TEST_RUNNING_WORKERS_HPP

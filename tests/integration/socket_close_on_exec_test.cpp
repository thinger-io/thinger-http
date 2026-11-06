#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/client.hpp>
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <chrono>
#include <memory>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <libproc.h>
#include <sys/proc_info.h>
#include <sys/socket.h>
#endif
#include "../fixtures/transport_matrix.hpp"

// The sockets of the library are not inherited by the programs its process runs (fork and
// exec), as Go and libuv open every descriptor: listening, accepted and client sockets, over
// TCP, TLS and Unix sockets. asio opens them without SOCK_CLOEXEC, and a child launcher that
// closes what it does not need (close_range) cannot on kernels before 5.9

using namespace thinger;
using namespace std::chrono_literals;
using thinger::http::test::transport;
using thinger::http::test::transport_listener;

namespace {

    struct socket_descriptors {
        int sockets = 0;
        std::vector<int> inherited;
    };

    // Every socket descriptor of the process, and those a child would inherit
    socket_descriptors sweep_sockets() {
        socket_descriptors found;
        for (int descriptor = 0; descriptor < 1024; ++descriptor) {
            struct stat status{};
            if (::fstat(descriptor, &status) != 0 || !S_ISSOCK(status.st_mode)) continue;
#if defined(__APPLE__)
            // Not a socket of the library: getaddrinfo opens one to the kernel (com.apple.netsrc),
            // inherited by the children of any process that resolves a name
            socket_fdinfo info{};
            if (::proc_pidfdinfo(::getpid(), descriptor, PROC_PIDFDSOCKETINFO, &info, sizeof(info)) > 0 &&
                info.psi.soi_family == AF_SYSTEM) continue;
#endif
            ++found.sockets;
            if (!(::fcntl(descriptor, F_GETFD) & FD_CLOEXEC)) found.inherited.push_back(descriptor);
        }
        return found;
    }

}

TEST_CASE("Listening, accepted and client sockets are close-on-exec", "[socket][cloexec][integration]") {
    REQUIRE(sweep_sockets().sockets == 0);

    http::server server;
    server.get("/", [](http::request&, http::response& res) { res.send("ok"); });
    std::vector<std::unique_ptr<transport_listener>> listeners;
    for (auto kind : {transport::tcp, transport::tls, transport::unix}) {
        listeners.push_back(std::make_unique<transport_listener>(server, kind));
    }
    std::thread thread([&server] { server.wait(); });

    {
        // One client per transport, its connection kept open in its pool (and accepted by the
        // server) while the descriptors are swept
        std::vector<std::unique_ptr<http::client>> clients;
        for (const auto& listener : listeners) {
            auto& client = *clients.emplace_back(std::make_unique<http::client>());
            listener->endpoint().configure(client).timeout(10s);
            REQUIRE(client.get(listener->endpoint().url("/")).ok());
        }

        auto found = sweep_sockets();
        INFO("inherited socket descriptors: " << found.inherited.size());
        CHECK(found.sockets >= 9);
        CHECK(found.inherited.empty());
    }

    server.stop();
    thread.join();
}

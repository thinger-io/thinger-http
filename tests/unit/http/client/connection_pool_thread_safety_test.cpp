#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/connection_pool.hpp>
#include <thinger/http/client/client_connection.hpp>
#include <thinger/asio/sockets/tcp_socket.hpp>
#include <boost/asio.hpp>
#include <atomic>
#include <chrono>
#include <random>
#include <thread>
#include <vector>

using namespace thinger::http;
using namespace std::chrono_literals;
namespace asio = boost::asio;

namespace {

    // A connection that needs no server: reusable, and stale while idle, as the test says
    class fake_connection : public client_connection {
    public:
        explicit fake_connection(asio::io_context& context)
            : client_connection(std::make_shared<thinger::asio::tcp_socket>("test", context)) {}

        bool reusable() const override { return reusable_now; }
        bool stale() const override { return stale_now; }

        std::atomic<bool> reusable_now{true};
        std::atomic<bool> stale_now{false};
        // Set while a test thread uses it: never lent twice at the same time
        std::atomic<bool> in_use{false};
    };

    std::shared_ptr<fake_connection> fake(asio::io_context& context) {
        return std::make_shared<fake_connection>(context);
    }

    // Give a new connection back to the pool right away (as after its first request)
    std::shared_ptr<fake_connection> park(connection_pool& pool, asio::io_context& context, const std::string& key) {
        auto connection = fake(context);
        pool.add(key, key, connection).release();
        return connection;
    }

}

TEST_CASE("Connection pool keeps connections for reuse", "[connection_pool][unit]") {
    asio::io_context context;
    connection_pool pool;

    SECTION("A connection given back is lent again for the same key only") {
        auto connection = fake(context);
        auto lease = pool.add("a", "http://a:80", connection);
        REQUIRE(lease.connection() == connection);
        REQUIRE(pool.size() == 0);
        REQUIRE(pool.get_stats().active == 1);

        lease.release();
        REQUIRE_FALSE(lease);
        REQUIRE(pool.size() == 1);
        REQUIRE_FALSE(pool.acquire("b"));

        auto again = pool.acquire("a");
        REQUIRE(again.connection() == connection);
        REQUIRE(pool.size() == 0);

        auto stats = pool.get_stats();
        REQUIRE(stats.created == 1);
        REQUIRE(stats.reused == 1);
        REQUIRE(stats.active == 1);
        REQUIRE(stats.hosts["http://a:80"].active == 1);
        REQUIRE(stats.hosts["http://a:80"].idle == 0);
    }

    SECTION("A lease destroyed gives its connection back") {
        {
            auto lease = pool.add("a", "a", fake(context));
        }
        REQUIRE(pool.size() == 1);
        REQUIRE(pool.get_stats().active == 0);
    }

    SECTION("Connections that cannot carry another request are not kept") {
        auto connection = fake(context);
        connection->reusable_now = false;
        pool.add("a", "a", connection).release();
        REQUIRE(pool.size() == 0);
        REQUIRE(pool.get_stats().hosts.empty());
    }

    SECTION("The most recently used is lent first, skipping those closed meanwhile") {
        auto older = park(pool, context, "a");
        auto newer = park(pool, context, "a");
        REQUIRE(pool.size() == 2);

        newer->stale_now = true;
        auto lease = pool.acquire("a");
        REQUIRE(lease.connection() == older);
        REQUIRE(pool.size() == 0);
        REQUIRE_FALSE(pool.acquire("a"));
    }

    SECTION("Idle connections are bounded per key and in total, closing the oldest") {
        pool.configure({.max_idle_per_host = 2, .max_idle = 3, .idle_timeout = 10s});
        auto a1 = park(pool, context, "a");
        auto a2 = park(pool, context, "a");
        auto a3 = park(pool, context, "a");
        REQUIRE(pool.get_stats().hosts["a"].idle == 2);

        auto b1 = park(pool, context, "b");
        auto b2 = park(pool, context, "b");
        auto stats = pool.get_stats();
        REQUIRE(stats.idle == 3);
        REQUIRE(stats.hosts["a"].idle == 1);
        REQUIRE(stats.hosts["b"].idle == 2);

        // a1 was closed first (per key), then a2 (in total)
        auto lease = pool.acquire("a");
        REQUIRE(lease.connection() == a3);
        REQUIRE_FALSE(pool.acquire("a"));
    }

    SECTION("Lower limits apply to the connections already idle") {
        park(pool, context, "a");
        park(pool, context, "a");
        park(pool, context, "b");
        pool.configure({.max_idle_per_host = 1, .max_idle = 100, .idle_timeout = 10s});
        REQUIRE(pool.size() == 2);
        pool.configure({.max_idle_per_host = 0, .max_idle = 100, .idle_timeout = 10s});
        REQUIRE(pool.size() == 0);
    }

    SECTION("A pool with no idle connections allowed keeps none") {
        pool.configure({.max_idle_per_host = 0, .max_idle = 100, .idle_timeout = 10s});
        park(pool, context, "a");
        REQUIRE(pool.size() == 0);
        pool.configure({.max_idle_per_host = 8, .max_idle = 0, .idle_timeout = 10s});
        park(pool, context, "a");
        REQUIRE(pool.size() == 0);
    }

    SECTION("Idle connections expire, with their timer or when found expired") {
        pool.configure({.max_idle_per_host = 8, .max_idle = 100, .idle_timeout = 50ms});
        park(pool, context, "a");
        REQUIRE(pool.size() == 1);
        context.run_for(300ms);
        REQUIRE(pool.size() == 0);
        context.restart();

        // Without running the timer (e.g. a synchronous client between requests)
        park(pool, context, "a");
        std::this_thread::sleep_for(100ms);
        REQUIRE_FALSE(pool.acquire("a"));
        REQUIRE(pool.size() == 0);
    }

    SECTION("A connection lent again is not closed by its previous idle timer") {
        pool.configure({.max_idle_per_host = 8, .max_idle = 100, .idle_timeout = 50ms});
        auto connection = park(pool, context, "a");
        auto lease = pool.acquire("a");
        REQUIRE(lease);
        context.run_for(200ms);
        REQUIRE(lease.connection() == connection);
        REQUIRE(pool.get_stats().active == 1);
    }

    SECTION("Clearing the pool closes the idle connections only") {
        park(pool, context, "a");
        auto lease = pool.add("a", "a", fake(context));
        pool.clear();
        REQUIRE(pool.size() == 0);
        REQUIRE(pool.get_stats().active == 1);
        lease.release();
        REQUIRE(pool.size() == 1);
    }
}

TEST_CASE("Connection pool outlived by its leases", "[connection_pool][unit]") {
    asio::io_context context;
    auto connection = fake(context);
    connection_pool::lease lease;
    {
        connection_pool pool;
        lease = pool.add("a", "a", connection);
    }
    // Nothing to give it back to: it is closed
    lease.release();
    REQUIRE_FALSE(lease);
}

TEST_CASE("Connection pool thread safety", "[connection_pool][threading]") {
    asio::io_context context;
    connection_pool pool;
    pool.configure({.max_idle_per_host = 3, .max_idle = 10, .idle_timeout = 10s});

    const int num_threads = 8;
    const int operations_per_thread = 2000;
    std::atomic<int> shared_use{0};
    std::atomic<int> reused{0};

    auto worker = [&](int seed) {
        std::mt19937 random(static_cast<unsigned>(seed));
        std::uniform_int_distribution<> pick(0, 9);
        for (int i = 0; i < operations_per_thread; ++i) {
            std::string key = "host" + std::to_string(pick(random) % 5);
            auto lease = pool.acquire(key);
            if (lease) {
                reused++;
            } else {
                lease = pool.add(key, key, fake(context));
            }
            auto connection = std::static_pointer_cast<fake_connection>(lease.connection());
            if (connection->in_use.exchange(true)) shared_use++;
            // Some are not reusable, or stale when found
            if (pick(random) == 0) connection->reusable_now = false;
            if (pick(random) == 0) connection->stale_now = true;
            connection->in_use = false;
            lease.release();
        }
    };

    std::vector<std::thread> threads;
    for (int i = 0; i < num_threads; ++i) threads.emplace_back(worker, i);
    for (auto& thread : threads) thread.join();

    REQUIRE(shared_use == 0);
    REQUIRE(reused > 0);
    auto stats = pool.get_stats();
    REQUIRE(stats.active == 0);
    REQUIRE(stats.idle <= 10);
    for (const auto& [origin, host] : stats.hosts) REQUIRE(host.idle <= 3);
    REQUIRE(stats.reused == static_cast<size_t>(reused.load()));
}

#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/pool_server.hpp>
#include <thinger/http/server/server_standalone.hpp>
#include <thinger/util/logger.hpp>
#include <spdlog/sinks/ringbuffer_sink.h>
#include <boost/asio.hpp>

#include "server_test_helpers.hpp"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

// At info level a server logs what an operator needs, as Node and Go servers do: where it
// listens, when a graceful shutdown starts (with its connections and grace period), and when
// it stopped. Its threads and the internals of starting and stopping log at debug level

using namespace thinger;
using namespace std::chrono_literals;

namespace {

    // Every message of the library logger, as "[level] message", while alive
    struct log_capture {
        std::shared_ptr<spdlog::logger> previous = logging::get_logger();
        std::shared_ptr<spdlog::sinks::ringbuffer_sink_mt> sink =
            std::make_shared<spdlog::sinks::ringbuffer_sink_mt>(4096);

        log_capture() {
            auto logger = std::make_shared<spdlog::logger>("http", sink);
            logger->set_pattern("[%l] %v");
            logger->set_level(spdlog::level::trace);
            logging::set_logger(logger);
        }
        ~log_capture() { logging::set_logger(previous); }

        std::vector<std::string> lines() const { return sink->last_formatted(); }

        // The messages of a level, without the level and the line ending
        std::vector<std::string> at(const std::string& level) const {
            std::vector<std::string> result;
            std::string prefix = "[" + level + "] ";
            for (auto line : lines()) {
                if (!line.starts_with(prefix)) continue;
                line = line.substr(prefix.size());
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                result.push_back(line);
            }
            return result;
        }

        size_t count(const std::string& level, const std::string& text) const {
            auto messages = at(level);
            return std::count_if(messages.begin(), messages.end(),
                                 [&](const std::string& message) { return message.find(text) != std::string::npos; });
        }

        std::string dump() const {
            std::string out;
            for (const auto& line : lines()) out += line;
            return out;
        }
    };

}

TEST_CASE("A pool_server logs one line to listen and one to start and end a shutdown", "[logging][shutdown][integration]") {
    log_capture logs;
    std::string url;
    {
        http::pool_server server;
        REQUIRE(server.listen("127.0.0.1", 0));
        url = "http://127.0.0.1:" + std::to_string(server.local_port());
        REQUIRE(server.shutdown(1s));
        // Later calls wait for the same end, without logging it again
        server.begin_shutdown(1s);
        server.stop();
    }
    INFO("captured logs:\n" << logs.dump());

    std::vector<std::string> expected = {
        "listening on " + url,
        "shutting down the server on " + url + ": 0 connections open, grace period of 1000 ms",
        "server on " + url + " stopped",
    };
    CHECK(logs.at("info") == expected);
    // The internals are still there, for debugging
    CHECK(logs.count("debug", "stopping worker thread") >= 1);
}

TEST_CASE("A server stopped without a shutdown logs that it stopped, once", "[logging][integration]") {
    log_capture logs;
    std::string url;
    {
        http::server server;
        REQUIRE(server.listen("127.0.0.1", 0));
        url = "http://127.0.0.1:" + std::to_string(server.local_port());
        server.stop();
        server.stop();
    }
    INFO("captured logs:\n" << logs.dump());

    std::vector<std::string> expected = {"listening on " + url, "server on " + url + " stopped"};
    CHECK(logs.at("info") == expected);
}

TEST_CASE("A server logs no line per connection at info", "[logging][integration]") {
    log_capture logs;
    std::string url;
    {
        http::server server;
        server_test::running_server running(server);
        url = "http://127.0.0.1:" + std::to_string(running.port);

        boost::asio::io_context context;
        boost::asio::ip::tcp::socket connection(context);
        connection.connect({boost::asio::ip::make_address("127.0.0.1"), running.port});
        auto deadline = std::chrono::steady_clock::now() + 3s;
        while (server.stats().total_connections == 0 && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(5ms);
        }
        REQUIRE(server.stats().total_connections == 1);
    }
    INFO("captured logs:\n" << logs.dump());

    std::vector<std::string> expected = {"listening on " + url, "server on " + url + " stopped"};
    CHECK(logs.at("info") == expected);
    CHECK(logs.count("debug", "received connection from") == 1);
}

TEST_CASE("Each server names where it listens in its logs", "[logging][integration]") {
    log_capture logs;

    SECTION("HTTPS") {
        http::pool_server server;
        server.enable_ssl(true);
        REQUIRE(server.listen("127.0.0.1", 0));
        auto url = "https://127.0.0.1:" + std::to_string(server.local_port());
        server.stop();
        INFO("captured logs:\n" << logs.dump());
        CHECK(logs.count("info", "listening on " + url) == 1);
        CHECK(logs.count("info", "server on " + url + " stopped") == 1);
    }

    SECTION("Unix socket") {
        auto path = (std::filesystem::temp_directory_path() /
                     ("thinger_logs_" + std::to_string(::getpid()) + ".sock")).string();
        http::server server;
        REQUIRE(server.listen_unix(path));
        server.stop();
        INFO("captured logs:\n" << logs.dump());
        CHECK(logs.count("info", "listening on unix:" + path) == 1);
        CHECK(logs.count("info", "server on unix:" + path + " stopped") == 1);
    }

    SECTION("A server shut down before it listened") {
        http::server server;
        server.begin_shutdown(1s);
        INFO("captured logs:\n" << logs.dump());
        CHECK(logs.count("info", "shutting down the server: 0 connections open") == 1);
    }
}

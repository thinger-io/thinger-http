#include "server_test_helpers.hpp"
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>

using namespace thinger;
using namespace std::chrono_literals;

// Paths of static files percent-decoded (spaces, UTF-8 names, "+" literal, "%2F" separating
// directories, a NUL refused), and encoded traversal attempts kept inside the directory: by
// the request parser over TCP, and by the containment check of the file server when the
// request reaches it unparsed (dispatched in memory)

namespace {

    namespace fs = std::filesystem;
    using namespace server_test;

    constexpr const char* secret = "top secret";
    constexpr const char* index_page = "<html>index</html>";

    struct raw_response {
        int status = 0;
        std::string body;
    };

    // Send a request (closing the connection after it) and read its response, within 3 s
    raw_response get(uint16_t port, const std::string& target) {
        auto result = raw_exchange(port, "GET " + target + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n");
        INFO(target << "\n" << result.data);
        REQUIRE(result.closed);
        auto end_headers = result.data.find("\r\n\r\n");
        REQUIRE(end_headers != std::string::npos);
        return {std::stoi(result.data.substr(9, 3)), result.data.substr(end_headers + 4)};
    }

    // The same request dispatched in memory: not parsed, so its path reaches the file server as sent
    raw_response dispatch(http::server& server, const std::string& target) {
        auto response = run_dispatch(server, make_request(http::method::GET, target));
        REQUIRE(response);
        return {static_cast<int>(response->get_status_code()), response->get_content()};
    }

    void write_file(const fs::path& path, const std::string& content) {
        fs::create_directories(path.parent_path());
        std::ofstream(path, std::ios::binary | std::ios::trunc) << content;
    }

    // A served folder (www, with index.html as its fallback) and secret.txt beside it
    struct static_site {
        fs::path base;
        fs::path root;
        http::server server;
        std::unique_ptr<running_server> running;
        uint16_t port = 0;

        static_site() {
            static std::atomic<int> sites{0};
            base = fs::temp_directory_path() /
                   ("thinger_static_paths_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
                    "_" + std::to_string(sites++));
            root = base / "www";
            write_file(root / "index.html", index_page);
            write_file(root / "my file.png", "png bytes");
            write_file(root / "informe-\xC3\xB1.pdf", "pdf bytes");
            write_file(root / "a+b.txt", "plus");
            write_file(root / "a b.txt", "space");
            write_file(root / "sub" / "file.txt", "in sub");
            write_file(root / "file2.txt", "file two");
            write_file(base / "secret.txt", secret);
            server.serve_static("/static", root.string());
            server.get("/echo/:name", [](http::request& req, http::response& res) { res.send(req["name"]); });
            running = std::make_unique<running_server>(server);
            port = running->port;
        }

        ~static_site() {
            running.reset();
            std::error_code ec;
            fs::remove_all(base, ec);
        }
    };

    // Answered 400 (refused by the request parser) or 403 (outside the directory): never the
    // secret, a file or the fallback
    void require_refused(const raw_response& response) {
        CHECK((response.status == 400 || response.status == 403));
        CHECK(response.body.find(secret) == std::string::npos);
        CHECK(response.body.find(index_page) == std::string::npos);
    }

}

TEST_CASE("Static file paths are percent-decoded", "[server][static][integration]") {
    static_site site;

    SECTION("A space") {
        auto response = get(site.port, "/static/my%20file.png");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "png bytes");
    }

    SECTION("A UTF-8 name") {
        auto response = get(site.port, "/static/informe-%C3%B1.pdf");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "pdf bytes");
    }

    SECTION("\"+\" is literal, not a space, sent as it is or encoded") {
        for (const auto* target : {"/static/a+b.txt", "/static/a%2Bb.txt", "/static/a+%62.txt", "/static/a%2bb.txt"}) {
            INFO(target);
            auto response = get(site.port, target);
            REQUIRE(response.status == 200);
            REQUIRE(response.body == "plus");
        }
        REQUIRE(get(site.port, "/static/a%20b.txt").body == "space");
    }

    SECTION("A decoded \"/\" separates directories") {
        auto response = get(site.port, "/static/sub%2Ffile.txt");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "in sub");
        REQUIRE(get(site.port, "/static/sub%2ffile.txt").body == "in sub");
    }

    SECTION("A decoded NUL: 400, neither a file nor the fallback") {
        for (const auto* target : {"/static/a%00.txt", "/static/a+b.txt%00", "/static/%00"}) {
            INFO(target);
            auto response = get(site.port, target);
            REQUIRE(response.status == 400);
            REQUIRE(response.body.find("plus") == std::string::npos);
            REQUIRE(response.body.find(index_page) == std::string::npos);
        }
    }

    SECTION("An encoded name without a file: the fallback") {
        auto response = get(site.port, "/static/no%20existe-%C3%B1.html");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == index_page);
    }

    SECTION("The same answers dispatched in memory") {
        REQUIRE(dispatch(site.server, "/static/my%20file.png").body == "png bytes");
        REQUIRE(dispatch(site.server, "/static/a%2Bb.txt").body == "plus");
        REQUIRE(dispatch(site.server, "/static/sub%2Ffile.txt").body == "in sub");
        REQUIRE(dispatch(site.server, "/static/a%00.txt").status == 400);
    }

    SECTION("Path parameters of other routes stay raw") {
        auto response = get(site.port, "/echo/my%20file+x");
        REQUIRE(response.status == 200);
        REQUIRE(response.body == "my%20file+x");
    }
}

TEST_CASE("Encoded traversal never leaves the static directory", "[server][static][integration]") {
    static_site site;
    const char* escapes[] = {
        "/static/%2E%2E/secret.txt",
        "/static/%2e%2e%2fsecret.txt",
        "/static/..%2Fsecret.txt",
        "/static/a%2F..%2F..%2Fsecret.txt",
        "/static/sub%2F..%2F..%2Fsecret.txt",
        "/static/%2E%2E%2F%2E%2E%2Fetc%2Fpasswd",
    };

    SECTION("Encoded dot segments: 400 or 403") {
        for (const auto* target : escapes) {
            INFO(target);
            require_refused(get(site.port, target));
        }
    }

    SECTION("Reaching the file server unparsed, the containment check refuses them: 403") {
        for (const auto* target : escapes) {
            INFO(target);
            auto response = dispatch(site.server, target);
            REQUIRE(response.status == 403);
            REQUIRE(response.body.find(secret) == std::string::npos);
        }
    }

    SECTION("An encoded absolute path: 403") {
        // ("/static/%2F<base>%2Fsecret.txt" decodes to the absolute path of the secret)
        auto absolute = (site.base / "secret.txt").string();
        std::string target = "/static/";
        for (char c : absolute) {
            if (c == '/') target += "%2F";
            else target += c;
        }
        auto response = get(site.port, target);
        REQUIRE(response.status == 403);
        REQUIRE(response.body.find(secret) == std::string::npos);
    }

    SECTION("Dot segments resolving inside the directory stay inside") {
        // Refused by the request parser over TCP, or the file inside it
        auto response = get(site.port, "/static/sub%2F..%2Ffile2.txt");
        CHECK((response.status == 400 || response.body == "file two"));
        CHECK(response.body.find(secret) == std::string::npos);
        auto unparsed = dispatch(site.server, "/static/sub%2F..%2Ffile2.txt");
        REQUIRE(unparsed.status == 200);
        REQUIRE(unparsed.body == "file two");
    }

    SECTION("A symlink leaving the directory, by its encoded name: 403") {
        fs::create_symlink(site.base / "secret.txt", site.root / "my secret.txt");
        fs::create_directory_symlink(site.base, site.root / "outside dir");
        for (const auto* target : {"/static/my%20secret.txt", "/static/outside%20dir%2Fsecret.txt",
                                   "/static/outside%20dir/secret.txt"}) {
            INFO(target);
            auto response = get(site.port, target);
            REQUIRE(response.status == 403);
            REQUIRE(response.body.find(secret) == std::string::npos);
        }
    }
}

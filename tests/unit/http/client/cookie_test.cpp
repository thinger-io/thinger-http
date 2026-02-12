#include <catch2/catch_test_macros.hpp>
#include <thinger/http/client/cookie.hpp>
#include <thinger/http/client/cookie_store.hpp>
#include <chrono>

using namespace thinger::http;

// ==================== Cookie Parsing Tests ====================

TEST_CASE("Cookie Basic Parsing", "[cookie][unit]") {

    SECTION("parse simple name=value") {
        auto c = cookie::parse("session=abc123");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "session");
        REQUIRE(c.get_value() == "abc123");
    }

    SECTION("parse with spaces around equals") {
        auto c = cookie::parse("token = xyz789");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "token");
        REQUIRE(c.get_value() == "xyz789");
    }

    SECTION("parse empty value") {
        auto c = cookie::parse("empty=");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "empty");
        REQUIRE(c.get_value() == "");
    }

    SECTION("parse empty string returns invalid cookie") {
        auto c = cookie::parse("");
        REQUIRE_FALSE(c.is_valid());
    }

    SECTION("parse string without equals returns invalid cookie") {
        auto c = cookie::parse("no-equals-here");
        REQUIRE_FALSE(c.is_valid());
    }

    SECTION("parse value with special characters") {
        auto c = cookie::parse("data=hello%20world");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_value() == "hello%20world");
    }

    SECTION("parse value with equals sign") {
        auto c = cookie::parse("encoded=base64==data");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "encoded");
        REQUIRE(c.get_value() == "base64==data");
    }
}

TEST_CASE("Cookie Attribute Parsing", "[cookie][unit]") {

    SECTION("parse with Path attribute") {
        auto c = cookie::parse("id=123; Path=/api");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "id");
        REQUIRE(c.get_value() == "123");
        REQUIRE(c.get_path() == "/api");
    }

    SECTION("parse with Domain attribute") {
        auto c = cookie::parse("id=456; Domain=example.com");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_domain() == "example.com");
    }

    SECTION("parse with Secure flag") {
        auto c = cookie::parse("id=789; Secure");
        REQUIRE(c.is_valid());
        REQUIRE(c.is_secure() == true);
    }

    SECTION("parse without Secure flag") {
        auto c = cookie::parse("id=789");
        REQUIRE(c.is_secure() == false);
    }

    SECTION("parse with HttpOnly flag") {
        auto c = cookie::parse("id=abc; HttpOnly");
        REQUIRE(c.is_valid());
        REQUIRE(c.is_http_only() == true);
    }

    SECTION("parse without HttpOnly flag") {
        auto c = cookie::parse("id=abc");
        REQUIRE(c.is_http_only() == false);
    }

    SECTION("parse with Max-Age attribute") {
        auto c = cookie::parse("id=xyz; Max-Age=3600");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_max_age().has_value());
        REQUIRE(c.get_max_age().value() == 3600);
        // Expires should be computed from max-age
        REQUIRE(c.get_expires() > 0);
    }

    SECTION("parse with SameSite=Strict") {
        auto c = cookie::parse("id=123; SameSite=Strict");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_same_site() == same_site_policy::strict);
    }

    SECTION("parse with SameSite=Lax") {
        auto c = cookie::parse("id=123; SameSite=Lax");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_same_site() == same_site_policy::lax);
    }

    SECTION("parse with SameSite=None") {
        auto c = cookie::parse("id=123; SameSite=None");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_same_site() == same_site_policy::none);
    }

    SECTION("default SameSite is Lax") {
        auto c = cookie::parse("id=123");
        REQUIRE(c.get_same_site() == same_site_policy::lax);
    }

    SECTION("parse with all attributes") {
        auto c = cookie::parse("session=token123; Path=/app; Domain=.example.com; Max-Age=86400; Secure; HttpOnly; SameSite=Strict");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "session");
        REQUIRE(c.get_value() == "token123");
        REQUIRE(c.get_path() == "/app");
        REQUIRE(c.get_domain() == ".example.com");
        REQUIRE(c.get_max_age().value() == 86400);
        REQUIRE(c.is_secure() == true);
        REQUIRE(c.is_http_only() == true);
        REQUIRE(c.get_same_site() == same_site_policy::strict);
    }

    SECTION("parse case-insensitive attribute names") {
        auto c = cookie::parse("id=1; PATH=/test; DOMAIN=test.com; SECURE; HTTPONLY; SAMESITE=STRICT");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_path() == "/test");
        REQUIRE(c.get_domain() == "test.com");
        REQUIRE(c.is_secure() == true);
        REQUIRE(c.is_http_only() == true);
        REQUIRE(c.get_same_site() == same_site_policy::strict);
    }

    SECTION("parse mixed case attribute names") {
        auto c = cookie::parse("id=1; pAtH=/test; DoMaIn=test.com");
        REQUIRE(c.get_path() == "/test");
        REQUIRE(c.get_domain() == "test.com");
    }
}

TEST_CASE("Cookie Expiry Parsing", "[cookie][unit]") {

    SECTION("parse with Expires attribute (RFC 1123 format)") {
        auto c = cookie::parse("id=123; Expires=Wed, 09 Jun 2021 10:18:14 GMT");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_expires() > 0);
    }

    SECTION("Max-Age takes precedence over Expires") {
        // If both are specified, Max-Age should be used
        auto c = cookie::parse("id=123; Expires=Wed, 09 Jun 2021 10:18:14 GMT; Max-Age=3600");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_max_age().value() == 3600);
        // The expires should be recalculated from max-age (roughly now + 3600)
        auto now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
        // Allow 5 second tolerance
        REQUIRE(c.get_expires() >= now + 3595);
        REQUIRE(c.get_expires() <= now + 3605);
    }

    SECTION("zero Max-Age means delete cookie (expired)") {
        auto c = cookie::parse("id=123; Max-Age=0");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_max_age().value() == 0);
        // Cookie should be marked as expired
        REQUIRE(c.is_expired());
    }

    SECTION("negative Max-Age means expired") {
        auto c = cookie::parse("id=123; Max-Age=-1");
        REQUIRE(c.is_valid());
        REQUIRE(c.is_expired());
    }
}

// ==================== Cookie Constructor Tests ====================

TEST_CASE("Cookie Constructors", "[cookie][unit]") {

    SECTION("default constructor creates empty cookie") {
        cookie c;
        REQUIRE_FALSE(c.is_valid());
        REQUIRE(c.get_name().empty());
        REQUIRE(c.get_value().empty());
    }

    SECTION("name-value constructor") {
        cookie c("session", "token123");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "session");
        REQUIRE(c.get_value() == "token123");
    }
}

// ==================== Cookie Setter Tests ====================

TEST_CASE("Cookie Setters", "[cookie][unit]") {

    SECTION("setters return reference for chaining") {
        cookie c;
        c.set_name("test")
         .set_value("value")
         .set_path("/")
         .set_domain("example.com")
         .set_secure(true)
         .set_http_only(true)
         .set_same_site(same_site_policy::strict);

        REQUIRE(c.get_name() == "test");
        REQUIRE(c.get_value() == "value");
        REQUIRE(c.get_path() == "/");
        REQUIRE(c.get_domain() == "example.com");
        REQUIRE(c.is_secure() == true);
        REQUIRE(c.is_http_only() == true);
        REQUIRE(c.get_same_site() == same_site_policy::strict);
    }

    SECTION("set_expires") {
        cookie c("test", "value");
        int64_t expires = 1623234000;  // Some timestamp
        c.set_expires(expires);
        REQUIRE(c.get_expires() == expires);
    }

    SECTION("set_max_age") {
        cookie c("test", "value");
        c.set_max_age(7200);
        REQUIRE(c.get_max_age().has_value());
        REQUIRE(c.get_max_age().value() == 7200);
    }

    SECTION("clear_max_age with nullopt") {
        cookie c("test", "value");
        c.set_max_age(3600);
        REQUIRE(c.get_max_age().has_value());
        c.set_max_age(std::nullopt);
        REQUIRE_FALSE(c.get_max_age().has_value());
    }
}

// ==================== Cookie Validity Tests ====================

TEST_CASE("Cookie Validity", "[cookie][unit]") {

    SECTION("cookie with name is valid") {
        cookie c;
        c.set_name("test");
        REQUIRE(c.is_valid());
    }

    SECTION("cookie without name is invalid") {
        cookie c;
        c.set_value("value");
        REQUIRE_FALSE(c.is_valid());
    }

    SECTION("session cookie is not expired") {
        cookie c("session", "value");
        // No expiry set = session cookie
        REQUIRE_FALSE(c.is_expired());
    }

    SECTION("future expiry is not expired") {
        cookie c("test", "value");
        auto future = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now() + std::chrono::hours(1));
        c.set_expires(future);
        REQUIRE_FALSE(c.is_expired());
    }

    SECTION("past expiry is expired") {
        cookie c("test", "value");
        auto past = std::chrono::system_clock::to_time_t(
            std::chrono::system_clock::now() - std::chrono::hours(1));
        c.set_expires(past);
        REQUIRE(c.is_expired());
    }
}

// ==================== Cookie Serialization Tests ====================

TEST_CASE("Cookie Serialization", "[cookie][unit]") {

    SECTION("basic cookie to_string") {
        cookie c("name", "value");
        REQUIRE(c.to_string() == "name=value; SameSite=Lax");
    }

    SECTION("cookie with path to_string") {
        cookie c("name", "value");
        c.set_path("/api");
        std::string str = c.to_string();
        REQUIRE(str.find("Path=/api") != std::string::npos);
    }

    SECTION("cookie with domain to_string") {
        cookie c("name", "value");
        c.set_domain("example.com");
        std::string str = c.to_string();
        REQUIRE(str.find("Domain=example.com") != std::string::npos);
    }

    SECTION("cookie with secure to_string") {
        cookie c("name", "value");
        c.set_secure(true);
        std::string str = c.to_string();
        REQUIRE(str.find("; Secure") != std::string::npos);
    }

    SECTION("cookie with httponly to_string") {
        cookie c("name", "value");
        c.set_http_only(true);
        std::string str = c.to_string();
        REQUIRE(str.find("; HttpOnly") != std::string::npos);
    }

    SECTION("cookie with max-age to_string") {
        cookie c("name", "value");
        c.set_max_age(3600);
        std::string str = c.to_string();
        REQUIRE(str.find("Max-Age=3600") != std::string::npos);
    }

    SECTION("cookie with strict same-site to_string") {
        cookie c("name", "value");
        c.set_same_site(same_site_policy::strict);
        std::string str = c.to_string();
        REQUIRE(str.find("SameSite=Strict") != std::string::npos);
    }

    SECTION("cookie with none same-site to_string") {
        cookie c("name", "value");
        c.set_same_site(same_site_policy::none);
        std::string str = c.to_string();
        REQUIRE(str.find("SameSite=None") != std::string::npos);
    }

    SECTION("full cookie to_string") {
        cookie c("session", "abc123");
        c.set_path("/app")
         .set_domain("example.com")
         .set_max_age(3600)
         .set_secure(true)
         .set_http_only(true)
         .set_same_site(same_site_policy::strict);

        std::string str = c.to_string();
        REQUIRE(str.find("session=abc123") != std::string::npos);
        REQUIRE(str.find("Path=/app") != std::string::npos);
        REQUIRE(str.find("Domain=example.com") != std::string::npos);
        REQUIRE(str.find("Max-Age=3600") != std::string::npos);
        REQUIRE(str.find("Secure") != std::string::npos);
        REQUIRE(str.find("HttpOnly") != std::string::npos);
        REQUIRE(str.find("SameSite=Strict") != std::string::npos);
    }
}

// ==================== Cookie Store Tests ====================

TEST_CASE("Cookie Store Basic Operations", "[cookie_store][unit]") {

    SECTION("new store is empty") {
        cookie_store store;
        REQUIRE(store.empty());
        REQUIRE(store.size() == 0);
    }

    SECTION("set_cookie adds cookie") {
        cookie_store store;
        store.set_cookie("session", "token123");
        REQUIRE_FALSE(store.empty());
        REQUIRE(store.size() == 1);
        REQUIRE(store.has_cookie("session"));
    }

    SECTION("set_cookie with cookie object") {
        cookie_store store;
        cookie c("auth", "xyz");
        c.set_secure(true);
        store.set_cookie(c);

        REQUIRE(store.has_cookie("auth"));
        auto retrieved = store.get_cookie("auth");
        REQUIRE(retrieved.has_value());
        REQUIRE(retrieved->get_value() == "xyz");
        REQUIRE(retrieved->is_secure() == true);
    }

    SECTION("get_cookie returns nullopt for missing cookie") {
        cookie_store store;
        auto result = store.get_cookie("nonexistent");
        REQUIRE_FALSE(result.has_value());
    }

    SECTION("has_cookie returns false for missing cookie") {
        cookie_store store;
        REQUIRE_FALSE(store.has_cookie("nonexistent"));
    }

    SECTION("remove_cookie removes existing cookie") {
        cookie_store store;
        store.set_cookie("test", "value");
        REQUIRE(store.has_cookie("test"));
        store.remove_cookie("test");
        REQUIRE_FALSE(store.has_cookie("test"));
    }

    SECTION("remove_cookie on nonexistent cookie does nothing") {
        cookie_store store;
        store.remove_cookie("nonexistent");  // Should not throw
        REQUIRE(store.empty());
    }

    SECTION("clear removes all cookies") {
        cookie_store store;
        store.set_cookie("a", "1");
        store.set_cookie("b", "2");
        store.set_cookie("c", "3");
        REQUIRE(store.size() == 3);
        store.clear();
        REQUIRE(store.empty());
    }

    SECTION("updating cookie overwrites existing") {
        cookie_store store;
        store.set_cookie("session", "old_value");
        store.set_cookie("session", "new_value");
        REQUIRE(store.size() == 1);
        auto c = store.get_cookie("session");
        REQUIRE(c->get_value() == "new_value");
    }
}

TEST_CASE("Cookie Store Cookie String Generation", "[cookie_store][unit]") {

    SECTION("empty store returns empty string") {
        cookie_store store;
        REQUIRE(store.get_cookie_string().empty());
    }

    SECTION("single cookie") {
        cookie_store store;
        store.set_cookie("session", "abc123");
        REQUIRE(store.get_cookie_string() == "session=abc123");
    }

    SECTION("multiple cookies separated by semicolon") {
        cookie_store store;
        store.set_cookie("a", "1");
        store.set_cookie("b", "2");

        std::string result = store.get_cookie_string();
        // Order is not guaranteed in unordered_map, check both possibilities
        REQUIRE((result == "a=1; b=2" || result == "b=2; a=1"));
    }

    SECTION("cookie string contains all cookies") {
        cookie_store store;
        store.set_cookie("session", "token");
        store.set_cookie("user", "john");
        store.set_cookie("lang", "en");

        std::string result = store.get_cookie_string();
        REQUIRE(result.find("session=token") != std::string::npos);
        REQUIRE(result.find("user=john") != std::string::npos);
        REQUIRE(result.find("lang=en") != std::string::npos);
    }
}

TEST_CASE("Cookie Store Invalid Cookie Handling", "[cookie_store][unit]") {

    SECTION("invalid cookie not added to store") {
        cookie_store store;
        cookie invalid;  // No name = invalid
        store.set_cookie(invalid);
        REQUIRE(store.empty());
    }

    SECTION("empty name cookie not added") {
        cookie_store store;
        cookie c;
        c.set_value("value_without_name");
        store.set_cookie(c);
        REQUIRE(store.empty());
    }
}

// ==================== Real-World Cookie Examples ====================

TEST_CASE("Real-World Cookie Examples", "[cookie][unit]") {

    SECTION("Google Analytics cookie") {
        auto c = cookie::parse("_ga=GA1.2.123456789.1234567890; Path=/; Expires=Fri, 31 Dec 2025 23:59:59 GMT");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "_ga");
        REQUIRE(c.get_path() == "/");
    }

    SECTION("Session cookie with security flags") {
        auto c = cookie::parse("JSESSIONID=abc123def456; Path=/; HttpOnly; Secure");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "JSESSIONID");
        REQUIRE(c.is_http_only() == true);
        REQUIRE(c.is_secure() == true);
    }

    SECTION("OAuth state cookie") {
        auto c = cookie::parse("oauth_state=xyz789; Path=/oauth; Max-Age=600; SameSite=Lax; Secure");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "oauth_state");
        REQUIRE(c.get_path() == "/oauth");
        REQUIRE(c.get_max_age().value() == 600);
        REQUIRE(c.get_same_site() == same_site_policy::lax);
        REQUIRE(c.is_secure() == true);
    }

    SECTION("Cross-site cookie") {
        auto c = cookie::parse("__Host-session=token; Path=/; Secure; SameSite=None");
        REQUIRE(c.is_valid());
        REQUIRE(c.get_name() == "__Host-session");
        REQUIRE(c.get_same_site() == same_site_policy::none);
    }
}

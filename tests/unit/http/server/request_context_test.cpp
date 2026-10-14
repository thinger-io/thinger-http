#include <catch2/catch_test_macros.hpp>
#include <thinger/http/server/request_context.hpp>
#include <memory>
#include <new>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

using namespace thinger::http;

namespace {
    struct principal {
        std::string user;
        std::vector<std::string> scopes;
    };

    // Counts the instances destroyed (moved-from ones excluded)
    struct tracked {
        explicit tracked(int& destroyed, int id = 0) : destroyed(&destroyed), id(id) {}
        tracked(tracked&& other) noexcept : destroyed(std::exchange(other.destroyed, nullptr)), id(other.id) {}
        tracked(const tracked&) = delete;
        tracked& operator=(const tracked&) = delete;
        tracked& operator=(tracked&&) = delete;
        ~tracked() { if (destroyed) ++*destroyed; }
        int* destroyed;
        int id;
    };

    inline constexpr context_key<principal> principal_key{"principal"};
    inline constexpr context_key<std::string> trace_key{"trace"};
    inline constexpr context_key<std::string> tenant_key{"tenant"};
    inline constexpr context_key<int> number_key;
    inline constexpr context_key<std::unique_ptr<principal>> owned_key{"owned"};
    inline constexpr context_key<tracked> tracked_key{"tracked"};
}

// Keys are identities: copying one would make another key
static_assert(!std::is_copy_constructible_v<context_key<int>>);
static_assert(!std::is_copy_assignable_v<context_key<int>>);

TEST_CASE("Request context stores typed values", "[request_context][unit]") {
    int destroyed = 0;  // (declared first: it outlives the values in the context)
    request_context context;
    REQUIRE(context.empty());

    SECTION("A value is read with its type") {
        context.set(principal_key, principal{"alice", {"devices:read", "devices:write"}});
        const principal* who = context.get(principal_key);
        REQUIRE(who != nullptr);
        REQUIRE(who->user == "alice");
        REQUIRE(who->scopes == std::vector<std::string>{"devices:read", "devices:write"});
        REQUIRE(context.contains(principal_key));
        REQUIRE(context.size() == 1);
    }

    SECTION("A key without value reads as absent") {
        REQUIRE(context.get(principal_key) == nullptr);
        REQUIRE_FALSE(context.contains(principal_key));
        const request_context& read_only = context;
        REQUIRE(read_only.get(principal_key) == nullptr);
    }

    SECTION("Two values of the same type live under two keys") {
        context.set(trace_key, "trace-1");
        context.set(tenant_key, "acme");
        REQUIRE(*context.get(trace_key) == "trace-1");
        REQUIRE(*context.get(tenant_key) == "acme");
        REQUIRE(context.size() == 2);
    }

    SECTION("Keys with the same type and name are still different keys") {
        static constexpr context_key<std::string> other_trace_key{"trace"};
        context.set(trace_key, "mine");
        REQUIRE(context.get(other_trace_key) == nullptr);
        context.set(other_trace_key, "theirs");
        REQUIRE(*context.get(trace_key) == "mine");
        REQUIRE(*context.get(other_trace_key) == "theirs");
    }

    SECTION("Values are constructed in place and can be changed") {
        auto& value = context.emplace(principal_key, "bob", std::vector<std::string>{"admin"});
        REQUIRE(value.user == "bob");
        context.get(principal_key)->scopes.push_back("audit");
        REQUIRE(context.get(principal_key)->scopes.size() == 2);
        context.set(number_key, 41);
        ++*context.get(number_key);
        REQUIRE(*context.get(number_key) == 42);
    }

    SECTION("Setting a key again replaces its value, destroying the previous one") {
        context.emplace(tracked_key, destroyed, 1);
        context.emplace(tracked_key, destroyed, 2);
        REQUIRE(destroyed == 1);
        REQUIRE(context.get(tracked_key)->id == 2);
        REQUIRE(context.size() == 1);
    }

    SECTION("Move-only values are moved, never copied") {
        context.set(owned_key, std::make_unique<principal>(principal{"carol", {}}));
        auto* owned = context.get(owned_key);
        REQUIRE(owned != nullptr);
        REQUIRE((*owned)->user == "carol");

        auto taken = context.take(owned_key);
        REQUIRE(taken.has_value());
        REQUIRE((*taken)->user == "carol");
        REQUIRE(context.get(owned_key) == nullptr);
        REQUIRE_FALSE(context.take(owned_key).has_value());
    }

    SECTION("Values are removed and destroyed") {
        context.emplace(tracked_key, destroyed);
        context.set(trace_key, "trace-1");
        REQUIRE(context.erase(tracked_key));
        REQUIRE(destroyed == 1);
        REQUIRE_FALSE(context.erase(tracked_key));
        REQUIRE(*context.get(trace_key) == "trace-1");
        context.clear();
        REQUIRE(context.empty());
    }
}

TEST_CASE("Request context destroys its values once", "[request_context][unit]") {
    int destroyed = 0;
    {
        request_context context;
        context.emplace(tracked_key, destroyed);
        request_context moved(std::move(context));
        REQUIRE(moved.get(tracked_key) != nullptr);
        request_context assigned;
        assigned = std::move(moved);
        REQUIRE(destroyed == 0);
    }
    REQUIRE(destroyed == 1);
}

TEST_CASE("Request context copies its values", "[request_context][unit]") {
    request_context context;
    context.set(principal_key, principal{"alice", {"read"}});
    context.set(trace_key, "trace-1");

    SECTION("Copyable values are copied") {
        request_context copy(context);
        copy.get(principal_key)->user = "bob";
        REQUIRE(context.get(principal_key)->user == "alice");
        REQUIRE(copy.get(principal_key)->user == "bob");
        REQUIRE(*copy.get(trace_key) == "trace-1");

        request_context assigned;
        assigned.set(number_key, 1);
        assigned = context;
        REQUIRE(assigned.get(number_key) == nullptr);
        REQUIRE(assigned.get(principal_key)->user == "alice");
    }

    SECTION("A value that cannot be copied makes the copy throw, leaving the target unchanged") {
        context.set(owned_key, std::make_unique<principal>());
        REQUIRE_THROWS_AS(request_context(context), std::logic_error);

        request_context target;
        target.set(number_key, 7);
        REQUIRE_THROWS_AS(target = context, std::logic_error);
        REQUIRE(*target.get(number_key) == 7);
        REQUIRE(target.size() == 1);
    }
}

TEST_CASE("Request context never reads a value with another type", "[request_context][unit]") {
    // A key reused at the address of another of a different type (keys with automatic
    // storage) does not see the value of the previous one
    request_context context;
    alignas(context_key<std::string>) unsigned char storage[sizeof(context_key<std::string>)];
    auto* text_key = new (storage) context_key<std::string>("text");
    context.set(*text_key, "value");
    text_key->~context_key();
    static_assert(sizeof(context_key<int>) == sizeof(context_key<std::string>));
    auto* int_key = new (storage) context_key<int>("int");
    REQUIRE(context.get(*int_key) == nullptr);
    context.set(*int_key, 5);
    REQUIRE(*context.get(*int_key) == 5);
    int_key->~context_key();
}

#ifndef THINGER_HTTP_SERVER_REQUEST_CONTEXT_HPP
#define THINGER_HTTP_SERVER_REQUEST_CONTEXT_HPP

#include <cstddef>
#include <memory>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

namespace thinger::http {

// Key of a value in a request_context: it identifies the value and fixes its type. Keys are
// told apart by their address, not by their type or name, so two values of the same type
// live under two keys, and the keys of two libraries never collide (as Go's context keys of
// an unexported type). Define each key once, with static storage duration:
//   inline constexpr http::context_key<principal> principal_key{"principal"};
template <typename T>
class context_key {
    static_assert(std::is_object_v<T> && !std::is_array_v<T> && !std::is_const_v<T> && !std::is_volatile_v<T>,
                  "context values are objects stored by value: use a non-const, non-array object type");
public:
    // `name` is only for diagnostics: values are looked up by the key itself
    constexpr explicit context_key(const char* name = "") noexcept : name_(name) {}

    // A copy would be another key: pass keys by reference
    context_key(const context_key&) = delete;
    context_key& operator=(const context_key&) = delete;

    constexpr const char* name() const noexcept { return name_; }

private:
    const char* name_;
};

// Typed values of a request (see request::context()), such as the principal an authentication
// middleware stores for the route handler: as Express res.locals, Go context.WithValue or
// Rust http::Extensions, with a value per key. Empty until a value is set, it allocates
// nothing until then. Not synchronized: a request is processed by one coroutine at a time.
//
//   middleware: req.context().set(principal_key, principal{"alice", {"devices:read"}});
//   handler:    if (const principal* who = req.context().get(principal_key)) ...
class request_context {
public:
    request_context() noexcept = default;
    request_context(request_context&&) noexcept = default;
    request_context& operator=(request_context&&) noexcept = default;
    ~request_context() = default;

    // Copies every value (e.g. a dispatch_options passed by copy); throws std::logic_error,
    // leaving the target unchanged, if a value is of a type that cannot be copied
    request_context(const request_context& other);
    request_context& operator=(const request_context& other);

    // Store a value constructed in place from `args`, replacing (and destroying) the one of
    // the key, if any; returns the value stored
    template <typename T, typename... Args>
    T& emplace(const context_key<T>& key, Args&&... args) {
        std::unique_ptr<holder_base> value = std::make_unique<holder<T>>(std::in_place, std::forward<Args>(args)...);
        T& stored = static_cast<holder<T>&>(*value).value;
        if (entry* existing = find(&key, type_tag<T>())) {
            existing->value = std::move(value);
        } else {
            entries_.push_back(entry{&key, type_tag<T>(), key.name(), std::move(value)});
        }
        return stored;
    }

    // Store a value, replacing (and destroying) the one of the key, if any; returns the value stored
    template <typename T, typename U = T>
    T& set(const context_key<T>& key, U&& value) {
        return emplace(key, std::forward<U>(value));
    }

    // The value of the key, or null if it has none
    template <typename T>
    T* get(const context_key<T>& key) noexcept {
        entry* found = find(&key, type_tag<T>());
        return found ? &static_cast<holder<T>&>(*found->value).value : nullptr;
    }

    template <typename T>
    const T* get(const context_key<T>& key) const noexcept {
        return const_cast<request_context*>(this)->get(key);
    }

    template <typename T>
    bool contains(const context_key<T>& key) const noexcept {
        return get(key) != nullptr;
    }

    // Remove the value of the key and return it (empty if the key has none)
    template <typename T>
    std::optional<T> take(const context_key<T>& key) {
        auto it = position(&key, type_tag<T>());
        if (it == entries_.end()) return std::nullopt;
        std::optional<T> value{std::move(static_cast<holder<T>&>(*it->value).value)};
        entries_.erase(it);
        return value;
    }

    // Remove (and destroy) the value of the key; false if the key has none
    template <typename T>
    bool erase(const context_key<T>& key) {
        auto it = position(&key, type_tag<T>());
        if (it == entries_.end()) return false;
        entries_.erase(it);
        return true;
    }

    // Number of values stored
    std::size_t size() const noexcept { return entries_.size(); }
    bool empty() const noexcept { return entries_.empty(); }

    // Remove (and destroy) every value
    void clear() noexcept { entries_.clear(); }

private:
    struct holder_base {
        holder_base() = default;
        holder_base(const holder_base&) = delete;
        holder_base& operator=(const holder_base&) = delete;
        virtual ~holder_base() = default;
        // A copy of the value, or null if its type cannot be copied
        virtual std::unique_ptr<holder_base> clone() const = 0;
    };

    template <typename T>
    struct holder final : holder_base {
        template <typename... Args>
        explicit holder(std::in_place_t, Args&&... args) : value(std::forward<Args>(args)...) {}

        std::unique_ptr<holder_base> clone() const override {
            if constexpr (std::is_copy_constructible_v<T>) {
                return std::make_unique<holder<T>>(std::in_place, value);
            } else {
                return nullptr;
            }
        }

        T value;
    };

    struct entry {
        const void* key;
        // Type of the value, checked too, so a value is never read with another type (e.g.
        // after a key with automatic storage is reused at the same address)
        const void* type;
        const char* name;
        std::unique_ptr<holder_base> value;
    };

    template <typename T>
    static const void* type_tag() noexcept {
        static constexpr char tag = 0;
        return &tag;
    }

    std::vector<entry>::iterator position(const void* key, const void* type) noexcept {
        auto it = entries_.begin();
        while (it != entries_.end() && (it->key != key || it->type != type)) ++it;
        return it;
    }

    entry* find(const void* key, const void* type) noexcept {
        auto it = position(key, type);
        return it == entries_.end() ? nullptr : &*it;
    }

    // Few values per request: a vector searched linearly (empty, it allocates nothing)
    std::vector<entry> entries_;
};

} // namespace thinger::http

#endif // THINGER_HTTP_SERVER_REQUEST_CONTEXT_HPP

#include "certificate_manager.hpp"
#include "client_verify.hpp"
#include "../../util/ascii.hpp"
#include "../../util/logger.hpp"
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace thinger::asio {

namespace detail {

    // Values by host name: exact names, and wildcards "*.domain" for one more label (not empty)
    // in front of domain, stored lowercase. A name is looked up case-insensitively, a trailing
    // dot ignored, without a copy, in two probes at most: the exact name, then the wildcard of
    // its domain (the name less its first label), so an exact name wins and the cost does not
    // grow with the names stored. Not synchronized: the manager's mutex guards it
    template<typename T>
    class host_table {
        // Case-insensitive hash (FNV-1a of the lowercase name) and equality, transparent so a
        // string_view is looked up as it is
        struct ihash {
            using is_transparent = void;
            size_t operator()(std::string_view name) const noexcept {
                uint64_t hash = 14695981039346656037ULL;
                for (char c : name) hash = (hash ^ static_cast<unsigned char>(util::ascii::to_lower(c))) * 1099511628211ULL;
                return static_cast<size_t>(hash);
            }
        };
        struct iequal {
            using is_transparent = void;
            bool operator()(std::string_view a, std::string_view b) const noexcept { return util::ascii::iequals(a, b); }
        };
        using map = std::unordered_map<std::string, T, ihash, iequal>;

        static constexpr std::string_view WILDCARD = "*.";

        map exact_;
        // The wildcards, by their domain (the name less "*.")
        map wildcards_;

        // The map of a registered name, and its key there
        std::pair<map*, std::string_view> place(std::string_view name) {
            if (!name.starts_with(WILDCARD)) return {&exact_, name};
            return {&wildcards_, name.substr(WILDCARD.size())};
        }

    public:
        // The entry a name resolves to; its key is valid while the table is not changed
        struct match {
            const T* value = nullptr;   // null: no entry
            bool wildcard = false;
            std::string_view key;       // the exact name, or the domain of the wildcard
        };

        // `name` is lowercase, and a host name or a wildcard (see valid_host_name())
        void set(std::string_view name, T value) {
            auto [entries, key] = place(name);
            if (auto it = entries->find(key); it != entries->end()) {
                it->second = std::move(value);
            } else {
                entries->emplace(std::string(key), std::move(value));
            }
        }

        // Remove the entry registered as `name` ("*.domain" for a wildcard): false if none
        bool erase(std::string_view name) {
            auto [entries, key] = place(name);
            auto it = entries->find(key);
            if (it == entries->end()) return false;
            entries->erase(it);
            return true;
        }

        match find(std::string_view name) const {
            if (name.ends_with('.')) name.remove_suffix(1);
            if (auto it = exact_.find(name); it != exact_.end()) return {&it->second, false, it->first};
            auto dot = name.find('.');
            if (dot == std::string_view::npos || dot == 0) return {};
            if (auto it = wildcards_.find(name.substr(dot + 1)); it != wildcards_.end()) {
                return {&it->second, true, it->first};
            }
            return {};
        }

        // The names registered, "*.domain" for the wildcards
        std::set<std::string> names() const {
            std::set<std::string> result;
            for (const auto& entry : exact_) result.insert(entry.first);
            for (const auto& entry : wildcards_) result.insert(std::string(WILDCARD) + entry.first);
            return result;
        }

        template<typename F>
        void for_each_value(F&& f) const {
            for (const auto& entry : exact_) f(entry.second);
            for (const auto& entry : wildcards_) f(entry.second);
        }
    };

}

namespace {

    // Whether a context has some TLS 1.3 cipher suite (OpenSSL 3 drops the names it does not
    // know without failing)
    bool has_tls13_suites(SSL_CTX* handle) {
        auto* ciphers = SSL_CTX_get_ciphers(handle);
        for (int i = 0; i < sk_SSL_CIPHER_num(ciphers); ++i) {
            if (std::string_view(SSL_CIPHER_get_version(sk_SSL_CIPHER_value(ciphers, i))) == "TLSv1.3") return true;
        }
        return false;
    }

    int protocol_version(tls_version version) {
        switch (version) {
            case tls_version::tls1_0: return TLS1_VERSION;
            case tls_version::tls1_1: return TLS1_1_VERSION;
            case tls_version::tls1_2: return TLS1_2_VERSION;
            case tls_version::tls1_3: return TLS1_3_VERSION;
        }
        return 0;
    }

}

namespace {

    // Set the options on a context, the security level first (it decides which ciphers and
    // certificates are allowed): false (logged) at the first one OpenSSL refuses
    bool set_tls_options(SSL_CTX* handle, const tls_options& options) {
        if (options.security_level) {
            if (*options.security_level < 0 || *options.security_level > 5) {
                LOG_ERROR("invalid OpenSSL security level: {}", *options.security_level);
                return false;
            }
            SSL_CTX_set_security_level(handle, *options.security_level);
        }
        if (SSL_CTX_set_min_proto_version(handle, protocol_version(options.min_version)) != 1 ||
            SSL_CTX_set_max_proto_version(handle, protocol_version(options.max_version)) != 1) {
            LOG_ERROR("invalid TLS protocol versions");
            return false;
        }
        if (!options.ciphers.empty() && SSL_CTX_set_cipher_list(handle, options.ciphers.c_str()) != 1) {
            LOG_ERROR("invalid TLS cipher list: {}", options.ciphers);
            return false;
        }
        if (!options.ciphersuites.empty() &&
            (SSL_CTX_set_ciphersuites(handle, options.ciphersuites.c_str()) != 1 || !has_tls13_suites(handle))) {
            LOG_ERROR("invalid TLS 1.3 cipher suites: {}", options.ciphersuites);
            return false;
        }
        if (!options.groups.empty() && SSL_CTX_set1_groups_list(handle, options.groups.c_str()) != 1) {
            LOG_ERROR("invalid TLS groups: {}", options.groups);
            return false;
        }
        if (options.prefer_server_ciphers) {
            SSL_CTX_set_options(handle, SSL_OP_CIPHER_SERVER_PREFERENCE);
        } else {
            SSL_CTX_clear_options(handle, SSL_OP_CIPHER_SERVER_PREFERENCE);
        }
        return true;
    }

    // The TLS 1.2 (and older) ciphers of a handshake, in OpenSSL's format (empty if none)
    std::string tls12_cipher_list(const SSL* ssl) {
        std::string list;
        auto* ciphers = SSL_get_ciphers(ssl);
        for (int i = 0; i < sk_SSL_CIPHER_num(ciphers); ++i) {
            const SSL_CIPHER* cipher = sk_SSL_CIPHER_value(ciphers, i);
            if (std::string_view(SSL_CIPHER_get_version(cipher)) == "TLSv1.3") continue;
            if (!list.empty()) list += ':';
            list += SSL_CIPHER_get_name(cipher);
        }
        return list;
    }

    // Whether OpenSSL takes the options, tried on a context of its own
    bool valid_tls_options(const tls_options& options) {
        boost::asio::ssl::context test(boost::asio::ssl::context::tls_server);
        return set_tls_options(test.native_handle(), options);
    }

    // The session id context of every context of the manager
    constexpr std::string_view SESSION_ID_CONTEXT = "thinger-http";

    // The argument of the SNI callback of a context: the manager it goes to
    using manager_ref = std::weak_ptr<certificate_manager>;

    // Where a context keeps its manager_ref (on the heap), freed with the context
    int manager_index() {
        static const int index = SSL_CTX_get_ex_new_index(0, nullptr, nullptr, nullptr,
            [](void* /*parent*/, void* ref, CRYPTO_EX_DATA* /*data*/, int /*index*/, long /*argl*/, void* /*argp*/) {
                delete static_cast<manager_ref*>(ref);
            });
        return index;
    }

    // Whether two references are to the same manager (or both to none)
    bool same_manager(const manager_ref& a, const manager_ref& b) {
        return !a.owner_before(b) && !b.owner_before(a);
    }

    // The TLS 1.3 tickets OpenSSL sends after a handshake on a new context
    constexpr size_t DEFAULT_TICKETS = 2;

    // Write a session cache on a context, every part of it (re-enabled after "off", a context
    // is as OpenSSL creates it), only where it differs: other listeners may be handshaking on
    // it. Disabled needs the three: without a cache, TLS 1.3 tickets would still resume
    void write_session_cache(SSL_CTX* handle, const session_cache_options& options) {
        long mode = options.enabled ? SSL_SESS_CACHE_SERVER : SSL_SESS_CACHE_OFF;
        if (SSL_CTX_get_session_cache_mode(handle) != mode) SSL_CTX_set_session_cache_mode(handle, mode);
        if (options.enabled && SSL_CTX_sess_get_cache_size(handle) != static_cast<long>(options.size)) {
            SSL_CTX_sess_set_cache_size(handle, static_cast<long>(options.size));
        }
        bool no_ticket = (SSL_CTX_get_options(handle) & SSL_OP_NO_TICKET) != 0;
        if (options.enabled && no_ticket) SSL_CTX_clear_options(handle, SSL_OP_NO_TICKET);
        if (!options.enabled && !no_ticket) SSL_CTX_set_options(handle, SSL_OP_NO_TICKET);
        size_t tickets = options.enabled ? DEFAULT_TICKETS : 0;
        if (SSL_CTX_get_num_tickets(handle) != tickets) SSL_CTX_set_num_tickets(handle, tickets);
    }

    // The invalid_argument of the setters (the reason is logged where it is found)
    boost::system::error_code invalid_argument() {
        return boost::system::errc::make_error_code(boost::system::errc::invalid_argument);
    }

    // Where an SSL keeps the record of its handshake (see tls_handshake_record), from its ClientHello
    // (see client_hello_callback()) until the connection takes it
    int handshake_index() {
        static const int index = SSL_get_ex_new_index(0, nullptr, nullptr, nullptr,
            [](void* /*parent*/, void* record, CRYPTO_EX_DATA* /*data*/, int /*index*/, long /*argl*/, void* /*argp*/) {
                delete static_cast<tls_handshake_record*>(record);
            });
        return index;
    }

    tls_handshake_record* record_of(const SSL* ssl) {
        return static_cast<tls_handshake_record*>(SSL_get_ex_data(ssl, handshake_index()));
    }

    // The first host name of the server_name extension of a ClientHello (RFC 6066, 3), if any
    std::optional<std::string> client_hello_server_name(SSL* ssl) {
        const unsigned char* data = nullptr;
        size_t size = 0;
        if (SSL_client_hello_get0_ext(ssl, TLSEXT_TYPE_server_name, &data, &size) != 1 || size < 2) return std::nullopt;
        if (((size_t(data[0]) << 8) | data[1]) != size - 2) return std::nullopt;
        for (size_t i = 2; i + 3 <= size;) {
            size_t length = (size_t(data[i + 1]) << 8) | data[i + 2];
            if (i + 3 + length > size) return std::nullopt;
            if (data[i] == TLSEXT_NAMETYPE_host_name) return std::string(reinterpret_cast<const char*>(data) + i + 3, length);
            i += 3 + length;
        }
        return std::nullopt;
    }

    // Keep the host name the client asks for, in the record of the handshake: the SNI callback
    // gets the one of the session instead when a TLS 1.2 session is resumed (see
    // SSL_get_servername()), and the mode of the host asked for must apply (see
    // certificate_manager::set_client_verify())
    int client_hello_callback(SSL* ssl, int* alert, void* /*arg*/) {
        try {
            auto name = client_hello_server_name(ssl);
            // Called again after a HelloRetryRequest: the same record
            auto* record = record_of(ssl);
            if (!record) {
                auto fresh = std::make_unique<tls_handshake_record>();
                if (SSL_set_ex_data(ssl, handshake_index(), fresh.get()) != 1) throw std::bad_alloc();
                record = fresh.release();
            }
            record->has_name = name.has_value();
            record->name = std::move(name).value_or(std::string());
            util::ascii::to_lower(record->name);
            if (record->name.ends_with('.')) record->name.pop_back();
            return SSL_CLIENT_HELLO_SUCCESS;
        } catch (const std::exception&) {
            if (alert) *alert = SSL_AD_INTERNAL_ERROR;
            return SSL_CLIENT_HELLO_ERROR;
        }
    }

    // The host name the client of a handshake asked for (none if none)
    const char* requested_name(const SSL* ssl) {
        if (const auto* record = record_of(ssl); record && record->has_name) return record->name.c_str();
        return SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
    }

    // Password callback of the keys: none, so an encrypted key fails
    int no_password(char* /*buffer*/, int /*size*/, int /*rwflag*/, void* /*user_data*/) {
        return -1;
    }

    // Whether an error loading a key says it is not the key of the certificate
    bool key_mismatch(const boost::system::error_code& ec) {
        auto error = static_cast<unsigned long>(ec.value());
        return ec.category() == boost::asio::error::get_ssl_category() && ERR_GET_LIB(error) == ERR_LIB_X509 &&
               (ERR_GET_REASON(error) == X509_R_KEY_VALUES_MISMATCH || ERR_GET_REASON(error) == X509_R_KEY_TYPE_MISMATCH);
    }

    // Letters, digits, hyphens and underscores
    bool host_label_char(char c) {
        c = util::ascii::to_lower(c);
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
    }

    // A host name, or a wildcard (*.domain): labels of 1 to 63 characters, 253 at most
    bool valid_host_name(std::string_view hostname) {
        if (hostname.starts_with("*.")) hostname.remove_prefix(2);
        if (hostname.empty() || hostname.size() > 253) return false;
        size_t label = 0;
        for (char c : hostname) {
            if (c != '.') {
                if (!host_label_char(c) || ++label > 63) return false;
            } else if (std::exchange(label, 0) == 0) {
                return false;
            }
        }
        return label > 0;
    }

}

certificate_error use_certificate(boost::asio::ssl::context& context, std::string_view certificate_chain,
                                  std::string_view private_key) {
    boost::system::error_code ec;
    context.use_certificate_chain(boost::asio::buffer(certificate_chain.data(), certificate_chain.size()), ec);
    if (ec) {
        LOG_ERROR("cannot read the certificate chain: {}", ec.message());
        return certificate_error::invalid_certificate;
    }
    SSL_CTX_set_default_passwd_cb(context.native_handle(), no_password);
    context.use_private_key(boost::asio::buffer(private_key.data(), private_key.size()),
                            boost::asio::ssl::context::pem, ec);
    if (ec && key_mismatch(ec)) {
        LOG_ERROR("the private key is not the one of the certificate");
        return certificate_error::key_mismatch;
    }
    if (ec) {
        LOG_ERROR("cannot read the private key: {}", ec.message());
        return certificate_error::invalid_private_key;
    }
    // A key of another type than the certificate's is taken without error, for another
    // certificate: the key in use has no certificate then
    if (SSL_CTX_check_private_key(context.native_handle()) != 1) {
        ERR_clear_error();
        LOG_ERROR("the private key is not the one of the certificate");
        return certificate_error::key_mismatch;
    }
    return certificate_error::none;
}

bool apply_tls_options(boost::asio::ssl::context& context, const tls_options& options) {
    // Tried first, so a context is never left half set
    return valid_tls_options(options) && set_tls_options(context.native_handle(), options);
}

certificate_manager::certificate_manager()
    : ssl_contexts_(std::make_unique<detail::host_table<std::shared_ptr<boost::asio::ssl::context>>>())
    , client_verify_(std::make_unique<detail::host_table<client_verify>>()) {
}

certificate_manager::~certificate_manager() = default;

void certificate_manager::prepare_context(boost::asio::ssl::context& context) {
    SSL_CTX* handle = context.native_handle();
    auto self = weak_from_this();
    auto* ref = static_cast<manager_ref*>(SSL_CTX_get_ex_data(handle, manager_index()));
    // Prepared again for this manager, it keeps the reference handshakes may be reading
    if (!ref || !same_manager(*ref, self)) {
        auto* fresh = new manager_ref(std::move(self));
        if (SSL_CTX_set_ex_data(handle, manager_index(), fresh) == 1) {
            delete ref;
            ref = fresh;
        } else {
            delete fresh;
            LOG_ERROR("cannot keep the certificate manager of a context: its SNI is left as it was");
        }
    }
    SSL_CTX_set_tlsext_servername_callback(handle, sni_callback);
    SSL_CTX_set_tlsext_servername_arg(handle, ref);
    SSL_CTX_set_client_hello_cb(handle, client_hello_callback, nullptr);
    SSL_CTX_set_session_id_context(handle,
                                   reinterpret_cast<const unsigned char*>(SESSION_ID_CONTEXT.data()),
                                   SESSION_ID_CONTEXT.size());
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_base_ssl_context(const tls_options& options) {
    auto context = std::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tls_server);
    context->set_options(boost::asio::ssl::context::single_dh_use);
#ifdef SSL_OP_NO_RENEGOTIATION
    // No TLS 1.2 renegotiation (OpenSSL 1.1.1 allows it), as Go's servers: contexts passed in
    // ready-made keep their own options
    SSL_CTX_set_options(context->native_handle(), SSL_OP_NO_RENEGOTIATION);
#endif
    prepare_context(*context);
    // Validated when set (see set_tls_options()), so this fails only if OpenSSL changed its mind
    if (!apply_tls_options(*context, options)) {
        LOG_ERROR("cannot set the TLS options of a new context");
        return nullptr;
    }
    return context;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::create_ssl_context(
    const std::string& cert_chain,
    const std::string& private_key) {
    
    tls_options options;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        options = options_;
    }
    auto context = create_base_ssl_context(options);
    if (!context) return nullptr;
    if (use_certificate(*context, cert_chain, private_key) != certificate_error::none) return nullptr;
    return context;
}

boost::system::error_code certificate_manager::set_certificate(const std::string& hostname,
                                                              const std::string& certificate,
                                                              const std::string& private_key) {
    // Checked first: a name refused costs no context
    if (!valid_host_name(hostname)) {
        LOG_ERROR("cannot set the certificate of host '{}': not a host name", hostname);
        return invalid_argument();
    }
    return set_certificate(hostname, create_ssl_context(certificate, private_key));
}

boost::system::error_code certificate_manager::set_certificate(const std::string& hostname,
                                                              std::shared_ptr<boost::asio::ssl::context> ssl_context) {
    if (!valid_host_name(hostname) || !ssl_context) {
        LOG_ERROR("cannot set the certificate of host '{}': {}", hostname,
                  ssl_context ? "not a host name" : "no usable certificate");
        return invalid_argument();
    }
    std::string name = hostname;
    util::ascii::to_lower(name);
    LOG_INFO("Setting SSL certificate for domain: {}", name);
    // It may become the default one (default host)
    prepare_context(*ssl_context);

    std::lock_guard<std::mutex> lock(mutex_);
    apply_session_cache(*ssl_context);
    ssl_contexts_->set(name, ssl_context);

    if (util::ascii::iequals(default_host_, name)) {
        LOG_INFO("Overriding default SSL certificate for domain: {}", name);
        default_context_ = std::move(ssl_context);
    }
    
    return {};
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::get_certificate(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto match = ssl_contexts_->find(hostname);
    return match.value ? *match.value : nullptr;
}

bool certificate_manager::has_certificate(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_contexts_->find(hostname).value != nullptr;
}

bool certificate_manager::remove_certificate(const std::string& hostname) {
    bool removed = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        removed = ssl_contexts_->erase(hostname);
    }
    if (removed) LOG_INFO("Removed SSL certificate: {}", hostname);
    return removed;
}

boost::system::error_code certificate_manager::set_default_certificate(
    std::shared_ptr<boost::asio::ssl::context> ssl_context) {
    // A null one is refused, not taken as "no default": a reload whose context failed to load
    // keeps the current certificate, and dropping it is remove_default_certificate()'s job
    if (!ssl_context) {
        LOG_ERROR("cannot set the default certificate: no context (remove_default_certificate() drops it)");
        return invalid_argument();
    }
    LOG_INFO("Setting default SSL certificate");
    prepare_context(*ssl_context);
    std::lock_guard<std::mutex> lock(mutex_);
    apply_session_cache(*ssl_context);
    default_context_ = std::move(ssl_context);
    self_signed_default_ = true;
    return {};
}

boost::system::error_code certificate_manager::set_default_certificate(const std::string& certificate,
                                                                      const std::string& private_key) {
    auto context = create_ssl_context(certificate, private_key);
    if (!context) return invalid_argument();
    return set_default_certificate(std::move(context));
}

void certificate_manager::remove_default_certificate() {
    LOG_INFO("Removing the default SSL certificate");
    std::lock_guard<std::mutex> lock(mutex_);
    default_context_.reset();
    self_signed_default_ = false;
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::get_default_certificate() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    // If no default certificate is set, generate a self-signed one
    if (!default_context_ && self_signed_default_) {
        LOG_WARNING("No default SSL certificate configured, generating self-signed certificate for development use");
        const_cast<certificate_manager*>(this)->generate_self_signed_certificate();
    }
    
    return default_context_;
}

void certificate_manager::set_default_host(const std::string& host) {
    std::lock_guard<std::mutex> lock(mutex_);
    default_host_ = host;
}

std::string certificate_manager::get_default_host() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return default_host_;
}

std::set<std::string> certificate_manager::get_registered_hosts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return ssl_contexts_->names();
}

boost::system::error_code certificate_manager::set_client_verify(const std::string& hostname, client_verify mode) {
    if (!valid_host_name(hostname)) {
        LOG_ERROR("invalid host name for client certificates: {}", hostname);
        return invalid_argument();
    }
    std::string name = hostname;
    util::ascii::to_lower(name);
    LOG_INFO("Client certificates of {}: {}", name, static_cast<int>(mode));
    std::lock_guard<std::mutex> lock(mutex_);
    client_verify_->set(name, mode);
    return {};
}

bool certificate_manager::remove_client_verify(const std::string& hostname) {
    std::lock_guard<std::mutex> lock(mutex_);
    return client_verify_->erase(hostname);
}

std::optional<client_verify> certificate_manager::get_client_verify(const std::string& hostname) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto match = client_verify_->find(hostname);
    if (!match.value) return std::nullopt;
    return *match.value;
}

void certificate_manager::apply_client_verify(SSL* ssl, const char* hostname) const {
    auto mode = hostname ? get_client_verify(hostname) : std::nullopt;
    const auto* policy = client_verify_policy::of(ssl);
    if (auto* record = record_of(ssl)) {
        record->listener_mode = policy ? policy->options().mode : client_verify::none;
        record->mode = mode.value_or(record->listener_mode);
        record->manager = weak_from_this();
    }
    if (!policy && !mode) return;
    if (!policy) policy = &client_verify_policy::deferred();
    policy->set_mode(ssl, mode.value_or(policy->options().mode));
}

bool certificate_manager::misdirected(const tls_handshake_record& handshake, std::string_view host) const {
    std::lock_guard<std::mutex> lock(mutex_);
    auto mode = client_verify_->find(host);
    if ((mode.value ? *mode.value : handshake.listener_mode) != handshake.mode) return true;
    // Entries compared by their node: the same one for the same registered name, whatever
    // certificate it holds now (none for both: the default)
    return handshake.has_name && ssl_contexts_->find(host).value != ssl_contexts_->find(handshake.name).value;
}

bool tls_handshake_record::misdirected(std::string_view host) const {
    if (host.ends_with('.')) host.remove_suffix(1);
    // Served whatever changed since the handshake, without a lookup
    if (has_name && util::ascii::iequals(host, name)) return false;
    // A manager gone since it chose leaves no table to compare: misdirected (fails closed)
    static const std::weak_ptr<const certificate_manager> none;
    bool chosen = manager.owner_before(none) || none.owner_before(manager);
    auto owner = manager.lock();
    if (owner ? !owner->misdirected(*this, host) : !chosen) return false;
    // At debug level: any client can send it
    LOG_DEBUG("misdirected request (421): host '{}' on a TLS connection for '{}'", host, has_name ? name : "(no SNI)");
    return true;
}

tls_handshake_record tls_handshake_record::take(SSL* ssl) {
    std::unique_ptr<tls_handshake_record> record(record_of(ssl));
    if (!record) return {};
    SSL_set_ex_data(ssl, handshake_index(), nullptr);
    return std::move(*record);
}

bool certificate_manager::update_tls_options(const tls_options& options) {
    // Tried on a context first, so the contexts created afterwards cannot fail
    if (!valid_tls_options(options)) return false;
    options_ = options;
    return true;
}

boost::system::error_code certificate_manager::set_tls_options(const tls_options& options) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!update_tls_options(options)) return invalid_argument();
    // They replace what the setters below changed
    legacy_.reset();
    ciphers_chosen_ = false;
    return {};
}

tls_options certificate_manager::get_tls_options() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return options_;
}

boost::system::error_code certificate_manager::set_server_ciphers(const std::string& ciphers,
                                                                 bool prefer_server_ciphers) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto options = options_;
    options.ciphers = ciphers;
    options.prefer_server_ciphers = prefer_server_ciphers;
    if (!update_tls_options(options)) return invalid_argument();
    ciphers_chosen_ = true;
    // chosen here: the legacy protocols no longer decide them
    if (legacy_) legacy_->ciphers.reset();
    return {};
}

void certificate_manager::enable_legacy_protocols(bool enable) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (enable == legacy_.has_value()) return;
    auto options = options_;
    if (enable) {
        legacy_change change{options.min_version, options.security_level, std::nullopt};
        options.min_version = tls_version::tls1_0;
        options.security_level = 0;
        // TLS 1.0 and 1.1 have no AEAD cipher: OpenSSL's defaults, unless they were chosen
        if (!ciphers_chosen_) {
            change.ciphers = options.ciphers;
            options.ciphers.clear();
        }
        if (update_tls_options(options)) legacy_ = std::move(change);
    } else {
        // What enabling them changed, and only that
        options.min_version = legacy_->min_version;
        options.security_level = legacy_->security_level;
        if (legacy_->ciphers) options.ciphers = *legacy_->ciphers;
        if (update_tls_options(options)) legacy_.reset();
    }
}

void certificate_manager::apply_session_cache(boost::asio::ssl::context& context) const {
    if (session_cache_) write_session_cache(context.native_handle(), session_cache_->options);
}

boost::system::error_code certificate_manager::claim_session_cache(const session_cache_options& options) {
    if (options.enabled &&
        (options.size == 0 || options.size > static_cast<size_t>(std::numeric_limits<long>::max()))) {
        LOG_ERROR("invalid TLS session cache size: {}", options.size);
        return invalid_argument();
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_cache_ && session_cache_->holders > 0 && session_cache_->options != options) {
        LOG_ERROR("the TLS session cache differs from the one of the other listeners of its certificate manager");
        return invalid_argument();
    }
    size_t holders = session_cache_ ? session_cache_->holders : 0;
    session_cache_ = session_cache_claim{options, holders + 1};
    if (default_context_) apply_session_cache(*default_context_);
    ssl_contexts_->for_each_value([this](const auto& context) {
        if (context) apply_session_cache(*context);
    });
    return {};
}

void certificate_manager::release_session_cache() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (session_cache_ && session_cache_->holders > 0) --session_cache_->holders;
}

void certificate_manager::generate_self_signed_certificate() {
    // Generate RSA key
    EVP_PKEY_CTX* ctx = EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr);
    EVP_PKEY* pkey = nullptr;
    
    if (ctx) {
        if (EVP_PKEY_keygen_init(ctx) > 0) {
            if (EVP_PKEY_CTX_set_rsa_keygen_bits(ctx, 2048) > 0) {
                EVP_PKEY_keygen(ctx, &pkey);
            }
        }
        EVP_PKEY_CTX_free(ctx);
    }
    
    if (!pkey) {
        LOG_ERROR("Failed to generate RSA key for self-signed certificate");
        return;
    }
    
    // Generate certificate
    X509* x509 = X509_new();
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 31536000L); // 1 year
    
    X509_set_pubkey(x509, pkey);
    
    // Subject, and issuer as it is self-signed: a name of its own, as recent OpenSSL versions
    // give the name of a certificate as const
    X509_NAME* name = X509_NAME_new();
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("US"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "O", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("Thinger Development"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, 
                              reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0);
    
    X509_set_subject_name(x509, name);
    X509_set_issuer_name(x509, name);
    X509_NAME_free(name);
    X509_sign(x509, pkey, EVP_sha256());
    
    // Create SSL context
    auto context = create_base_ssl_context(options_);
    if (!context) {
        X509_free(x509);
        EVP_PKEY_free(pkey);
        return;
    }
    
    // Set certificate and key
    if (SSL_CTX_use_certificate(context->native_handle(), x509) != 1) {
        LOG_ERROR("Failed to use certificate for self-signed cert");
    } else if (SSL_CTX_use_PrivateKey(context->native_handle(), pkey) != 1) {
        LOG_ERROR("Failed to use private key for self-signed cert");
    } else {
        apply_session_cache(*context);
        default_context_ = context;
        LOG_INFO("Generated self-signed certificate for development use (CN=localhost)");
    }
    
    // Cleanup
    X509_free(x509);
    EVP_PKEY_free(pkey);
}

std::shared_ptr<boost::asio::ssl::context> certificate_manager::select_context(const char* hostname) const {
    if (hostname != nullptr) {
        LOG_DEBUG("SNI connection for: {}", hostname);
        if (auto certificate = get_certificate(hostname)) return certificate;
        // At debug level: any client (or scanner) can send names without a certificate
        LOG_DEBUG("Using default server certificate for hostname: {}", hostname);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    return default_context_;
}

namespace {

    // Switch a handshake to the certificate of `context`. OpenSSL takes the certificate of the
    // context switched to, but also its cipher list and security level: the handshake keeps
    // those of the listener (as its versions and options), so a context given ready-made cannot
    // lower them. False if OpenSSL fails
    bool switch_context(SSL* ssl, SSL_CTX* context) {
        auto ciphers = tls12_cipher_list(ssl);
        int level = SSL_get_security_level(ssl);
        if (SSL_set_SSL_CTX(ssl, context) == nullptr ||
            (!ciphers.empty() && SSL_set_cipher_list(ssl, ciphers.c_str()) != 1)) {
            return false;
        }
        SSL_set_security_level(ssl, level);
        return true;
    }

}

// SNI callback implementation
int certificate_manager::sni_callback(SSL* ssl, int* al, void* arg) {
    if (!ssl || !arg) return SSL_TLSEXT_ERR_OK;

    // Get SNI server name (none if the client sent none)
    const char* hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);

    // Called from OpenSSL: nothing may be thrown through it
    try {
        // Kept alive while it chooses. Gone, nothing of it applies: the handshake stays on the
        // context it started with (that of its listener)
        auto manager = static_cast<const manager_ref*>(arg)->lock();
        if (!manager) return SSL_TLSEXT_ERR_OK;
        // The default certificate may have been replaced since the listener took it. The
        // connection holds a reference to the context it switches to, which outlives a later
        // replacement
        auto context = manager->select_context(hostname);
        if (context && context->native_handle() != SSL_get_SSL_CTX(ssl) &&
            !switch_context(ssl, context->native_handle())) {
            LOG_ERROR("cannot set the certificate for hostname {}", hostname ? hostname : "");
            if (al) *al = SSL_AD_INTERNAL_ERROR;
            return SSL_TLSEXT_ERR_ALERT_FATAL;
        }
        manager->apply_client_verify(ssl, requested_name(ssl));
    } catch (const std::exception& e) {
        LOG_ERROR("cannot select the certificate for hostname {}: {}", hostname ? hostname : "", e.what());
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    }
    
    return SSL_TLSEXT_ERR_OK;
}

} // namespace thinger::asio
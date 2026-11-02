#include "listener.hpp"
#include "async_event.hpp"
#include "detail/listener_access.hpp"
#include "io_context_of.hpp"
#include "with_timeout.hpp"
#include "worker_thread.hpp"
#include "sockets/ssl_socket.hpp"
#include "sockets/tcp_socket.hpp"
#include "sockets/unix_socket.hpp"
#include "ssl/certificate_manager.hpp"
#include "ssl/client_verify.hpp"
#include "../util/logger.hpp"
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <exception>
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/local/stream_protocol.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/steady_timer.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/this_coro.hpp>
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

namespace thinger::asio {

namespace {

using tcp = boost::asio::ip::tcp;
using local = boost::asio::local::stream_protocol;
using steady = std::chrono::steady_clock;

// Time between attempts to bind (see listener_options::max_listening_attempts)
constexpr std::chrono::seconds LISTEN_RETRY_DELAY{5};
// Time before accepting again after an error accepting (e.g. out of descriptors)
constexpr std::chrono::seconds ACCEPT_RETRY_DELAY{1};

// An accepted descriptor on its way to the executor serving it, closed if it never gets there
class accepted_descriptor {
public:
    explicit accepted_descriptor(int descriptor) : descriptor_(descriptor) {}
    accepted_descriptor(accepted_descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    accepted_descriptor& operator=(accepted_descriptor&&) = delete;
    ~accepted_descriptor() { if (descriptor_ >= 0) ::close(descriptor_); }

    int get() const { return descriptor_; }
    int release() { return std::exchange(descriptor_, -1); }

private:
    int descriptor_;
};

// A socket of the listener is not inherited by the programs a handler runs (fork and exec): asio
// opens it without SOCK_CLOEXEC. The acceptor thread on Linux needs none (see accept_pending())
void close_on_exec(int descriptor) {
    ::fcntl(descriptor, F_SETFD, FD_CLOEXEC);
}

// Sets up and serves an accepted connection. Nothing doing it may throw out of an accept handler
// or a worker: it would end the process. What is thrown is logged, and only drops the connection
// (its socket is destroyed, and closed, while unwinding)
template<typename Function>
void serve_without_throwing(Function&& serve) noexcept {
    try {
        serve();
    } catch (const std::exception& e) {
        LOG_ERROR("cannot serve an accepted connection: {}", e.what());
    } catch (...) {
        LOG_ERROR("cannot serve an accepted connection");
    }
}

// The thread a listener accepts on (see detail::listener_access): stopped, and joined, before
// it is destroyed
struct thread_stopper {
    void operator()(worker_thread* thread) const {
        thread->stop();
        delete thread;
    }
};
using acceptor_thread_ptr = std::unique_ptr<worker_thread, thread_stopper>;

// The TLS of the connections of a listener, taken by open(): each one is created with this
// context (its handshake may switch to another certificate of the manager, see
// certificate_manager::sni_callback()) and asks for client certificates with this policy
struct tls_setup {
    std::shared_ptr<boost::asio::ssl::context> context;
    std::shared_ptr<const client_verify_policy> client_verify;
};

// The TLS handshakes in progress on one io_context serving a listener with a bound (see
// listener_options::max_pending_handshakes). Used only on its strand, where the handshakes it
// bounds run: the connections waiting for a slot wait for `slot_freed`, set whenever a slot is
// freed (all of them wake and check again: the order is not first come, first served)
struct handshake_gate {
    explicit handshake_gate(boost::asio::io_context& io_context)
        : strand(boost::asio::make_strand(io_context)), slot_freed(strand) {}

    boost::asio::any_io_executor strand;
    async_event slot_freed;
    size_t in_progress = 0;
};

// A slot of a gate, taken while a handshake runs: freed (waking the connections waiting for one)
// however the handshake ends
class handshake_slot {
public:
    explicit handshake_slot(std::shared_ptr<handshake_gate> gate) : gate_(std::move(gate)) { ++gate_->in_progress; }
    ~handshake_slot() {
        --gate_->in_progress;
        gate_->slot_freed.set();
    }
    handshake_slot(const handshake_slot&) = delete;
    handshake_slot& operator=(const handshake_slot&) = delete;

private:
    std::shared_ptr<handshake_gate> gate_;
};

// A connection accepted whose handshake has not ended (waiting for a slot included): stop()
// closes it on the executor it handshakes on
struct pending_handshake {
    std::weak_ptr<tcp_socket> sock;
    boost::asio::any_io_executor executor;
    std::shared_ptr<handshake_gate> gate;   // none without a bound
};

// The handshake of a TLS connection, closed if it does not end by `deadline` (none: no limit)
awaitable<boost::system::error_code> handshake_until(std::shared_ptr<tcp_socket> sock,
                                                     std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (!deadline) co_return co_await sock->handshake();
    if (*deadline <= std::chrono::steady_clock::now()) co_return boost::asio::error::timed_out;
    // On the executor of the handshake. The handler does not own the socket
    auto timer = std::make_shared<boost::asio::steady_timer>(co_await boost::asio::this_coro::executor, *deadline);
    auto expired = std::make_shared<std::atomic<bool>>(false);
    timer->async_wait([weak = std::weak_ptr<tcp_socket>(sock), expired](const boost::system::error_code& ec) {
        if (ec) return;
        *expired = true;
        if (auto pending = weak.lock()) pending->close();
    });
    auto ec = co_await sock->handshake();
    timer->cancel();
    co_return *expired ? boost::system::error_code(boost::asio::error::timed_out) : ec;
}

// What a TLS connection throws is logged, as when a connection is served right away (see
// serve_without_throwing())
void log_serve_failure(const std::exception_ptr& error) {
    if (!error) return;
    try {
        std::rethrow_exception(error);
    } catch (const std::exception& e) {
        LOG_ERROR("cannot serve a TLS connection: {}", e.what());
    } catch (...) {
        LOG_ERROR("cannot serve a TLS connection");
    }
}

} // namespace

// What a listener shares with the handlers accepting and serving its connections: they hold
// it, never the listener, which may be gone by the time they run.
//
// Its mutex guards the short transitions only (stopping, the deliveries in progress, the
// handshakes pending, the gates, the acceptor's use by an accept handler), and is never held
// while the handler of the connections runs, nor while a handshake or a wait for a slot does:
// the handler may stop the listener, or destroy it. Nor while the executor providers run: they
// are the application's, and may block or call the listener (e.g. url()). Every connection reaches the handler
// through deliver(), which records the thread delivering it unless the listener is stopping;
// stop() sets stopping, closes the handshakes pending and waits until no delivery is recorded
// (its own excepted), so that once it returns no handler starts and none runs on another
// thread.
struct listener::state : std::enable_shared_from_this<state> {
    state(listener_options listener_options, executor_provider accept_executors, executor_provider serve_executors)
        : certificates(std::exchange(listener_options.tls, nullptr))
        , manager(certificates)
        , secure(certificates != nullptr)
        , options(std::move(listener_options))
        , accept_on(std::move(accept_executors))
        , serve_on(std::move(serve_executors))
        , allowed_remotes(options.allowed_remotes.begin(), options.allowed_remotes.end())
        , forbidden_remotes(options.forbidden_remotes.begin(), options.forbidden_remotes.end()) {}

    // The manager of the options (taken out of them), held until stop(), whatever open()
    // returns: the connections reach it through their contexts (see
    // certificate_manager::sni_callback()), so their host names keep their certificates and
    // client certificate modes while the listener runs, even if the application keeps none
    std::shared_ptr<certificate_manager> certificates;
    const std::weak_ptr<certificate_manager> manager;
    const bool secure;
    // The options, without the manager
    const listener_options options;
    const executor_provider accept_on;
    const executor_provider serve_on;
    const std::set<std::string> allowed_remotes;
    const std::set<std::string> forbidden_remotes;
    // Accept on a thread of its own (see detail::listener_access), set before open()
    bool own_acceptor_thread = false;

    mutable std::mutex mutex;
    // Notified when a delivery ends and when the listener stops
    std::condition_variable changed;
    // Written with the mutex held; read without it by the loops that only need to end
    std::atomic<bool> stopping{false};
    std::atomic<uint16_t> port{0};
    bool opened = false;
    bool started = false;
    std::string url;
    // The threads delivering a connection to the handler, one entry per delivery
    std::vector<std::thread::id> delivering;
    // The TLS connections from their accept to the end of their handshake; taken by stop()
    std::unordered_map<const tcp_socket*, pending_handshake> pending;
    // The gate of each io_context serving TLS connections, with a bound: created as they are
    // first used, kept until stop()
    std::unordered_map<boost::asio::io_context*, std::shared_ptr<handshake_gate>> gates;
    // Set by open(), with the mutex held, before anything accepts: no context without TLS
    tls_setup tls;
    // Whether it holds its session cache on the manager of `tls` (see open()): released once
    bool holds_session_cache = false;

    // Set by open(), with the mutex held, before anything accepts; taken by stop(). The thread
    // is declared before the acceptors, which use its io_context
    acceptor_thread_ptr acceptor_thread;
    std::unique_ptr<tcp::acceptor> tcp_acceptor;
    std::unique_ptr<local::acceptor> unix_acceptor;
    tcp tcp_protocol = tcp::v4();
    // Set by start(), before anything accepts
    sync_callback<void(std::shared_ptr<socket>)> on_connection;

    bool is_tcp() const { return std::holds_alternative<tcp_endpoint>(options.endpoint); }

    template<typename Protocol>
    std::unique_ptr<typename Protocol::acceptor>& acceptor_of() {
        if constexpr (std::is_same_v<Protocol, tcp>) return tcp_acceptor;
        else return unix_acceptor;
    }

    // The port of the options: 0 for Unix sockets
    uint16_t configured_port() const {
        const auto* where = std::get_if<tcp_endpoint>(&options.endpoint);
        return where ? where->port : 0;
    }

    // "http://host:port", "https://host:port" or "unix:/path"
    std::string url_for(uint16_t bound_port) const {
        if (const auto* unix = std::get_if<unix_endpoint>(&options.endpoint)) return "unix:" + unix->path;
        const auto& host = std::get<tcp_endpoint>(options.endpoint).host;
        auto shown = host.find(':') == std::string::npos ? host : "[" + host + "]";
        return (secure ? "https://" : "http://") + shown + ":" + std::to_string(bound_port);
    }

    // --- open() ---

    boost::system::error_code open() {
        std::shared_ptr<certificate_manager> tls;
        {
            std::scoped_lock lock(mutex);
            if (opened || stopping) return boost::asio::error::already_open;
            tls = certificates;
        }
        return open_with(tls);
    }

    boost::system::error_code open_with(const std::shared_ptr<certificate_manager>& tls) {
        tls_setup connections_tls;
        if (auto ec = prepare_tls(tls.get(), connections_tls)) return ec;
        if (auto ec = claim_session_cache(tls.get())) return ec;
        auto ec = open_claimed(std::move(connections_tls));
        // Nothing bound: the setting is left to other listeners
        if (ec) release_session_cache();
        return ec;
    }

    boost::system::error_code open_claimed(tls_setup connections_tls) {
        acceptor_thread_ptr thread;
        if (own_acceptor_thread) {
            thread.reset(new worker_thread(url_for(configured_port()) + " acceptor"));
            thread->start();
        }
        auto executor = thread ? boost::asio::any_io_executor(thread->get_io_context().get_executor())
                               : (accept_on ? accept_on() : boost::asio::any_io_executor());
        if (auto ec = check_executors(executor)) return ec;
        return is_tcp() ? open_on<tcp>(executor, std::move(thread), std::move(connections_tls))
                        : open_on<local>(executor, std::move(thread), std::move(connections_tls));
    }

    // The TLS of the connections, if any: the default certificate of the manager as it is now,
    // and the client certificate policy, whose trusted CAs must be readable
    boost::system::error_code prepare_tls(certificate_manager* tls, tls_setup& setup) const {
        if (!tls) {
            if (options.client_verify.mode != client_verify::none || !options.client_verify.trusted_cas.empty()) {
                LOG_WARNING("{} has no TLS: its client certificate options are ignored", url_for(configured_port()));
            }
            return {};
        }
        if (!is_tcp()) {
            LOG_ERROR("cannot listen on {}: TLS is served on TCP endpoints only", url_for(configured_port()));
            return boost::asio::error::invalid_argument;
        }
        setup.client_verify = client_verify_policy::create(options.client_verify);
        if (!setup.client_verify) {
            LOG_ERROR("cannot listen on {}: the trusted CAs of the client certificates cannot be read",
                      url_for(configured_port()));
            return boost::asio::error::invalid_argument;
        }
        setup.context = tls->get_default_certificate();
        if (!setup.context) {
            LOG_ERROR("cannot listen on {}: its certificate_manager has no default certificate", url_for(configured_port()));
            return boost::asio::error::invalid_argument;
        }
        return {};
    }

    // Apply the session cache of the options to the manager, before binding: refused if other
    // listeners of the same manager hold another one. Plain listeners have none
    boost::system::error_code claim_session_cache(certificate_manager* tls) {
        if (!tls) return {};
        if (auto ec = tls->claim_session_cache(options.session_cache)) {
            LOG_ERROR("cannot listen on {}: invalid TLS session cache", url_for(configured_port()));
            return ec;
        }
        std::scoped_lock lock(mutex);
        holds_session_cache = true;
        return {};
    }

    // Once, when open() fails or the listener stops: nothing if the manager is gone, and its
    // setting with it
    void release_session_cache() {
        {
            std::scoped_lock lock(mutex);
            if (!std::exchange(holds_session_cache, false)) return;
        }
        if (auto tls = manager.lock()) tls->release_session_cache();
    }

    // Both executors run on an io_context: the sockets of this release need one
    boost::system::error_code check_executors(const boost::asio::any_io_executor& accept_executor) const {
        if (!io_context_of(accept_executor)) {
            LOG_ERROR("cannot listen on {}: the executor to accept on does not run on an io_context", url_for(configured_port()));
            return boost::asio::error::invalid_argument;
        }
        if (!serve_on || !io_context_of(serve_on())) {
            LOG_ERROR("cannot listen on {}: the executor to serve on does not run on an io_context", url_for(configured_port()));
            return boost::asio::error::invalid_argument;
        }
        return {};
    }

    template<typename Protocol>
    boost::system::error_code open_on(const boost::asio::any_io_executor& executor, acceptor_thread_ptr thread,
                                      tls_setup connections_tls) {
        typename Protocol::endpoint endpoint;
        if (auto ec = endpoint_of(executor, endpoint)) return ec;
        std::unique_ptr<typename Protocol::acceptor> acceptor;
        if (auto ec = bind(executor, endpoint, acceptor)) return ec;
        return publish<Protocol>(std::move(acceptor), std::move(thread), std::move(connections_tls));
    }

    boost::system::error_code endpoint_of(const boost::asio::any_io_executor& executor, tcp::endpoint& endpoint) const {
        const auto& where = std::get<tcp_endpoint>(options.endpoint);
        boost::system::error_code ec;
        tcp::resolver resolver(executor);
        auto results = resolver.resolve(where.host, std::to_string(where.port), ec);
        if (!ec && results.empty()) ec = boost::asio::error::host_not_found;
        if (ec) {
            LOG_ERROR("cannot resolve {}:{}: {}", where.host, where.port, ec.message());
            return ec;
        }
        endpoint = results.begin()->endpoint();
        return {};
    }

    boost::system::error_code endpoint_of(const boost::asio::any_io_executor&, local::endpoint& endpoint) const {
        const auto& path = std::get<unix_endpoint>(options.endpoint).path;
        if (path.empty()) {
            LOG_ERROR("cannot listen on a Unix socket without a path");
            return boost::asio::error::invalid_argument;
        }
        // A socket file left by an earlier server is replaced
        std::error_code ec;
        std::filesystem::remove(path, ec);
        endpoint = local::endpoint(path);
        return {};
    }

    // Bind and listen, retrying every LISTEN_RETRY_DELAY up to max_listening_attempts
    template<typename Endpoint, typename Acceptor>
    boost::system::error_code bind(const boost::asio::any_io_executor& executor, const Endpoint& endpoint,
                                   std::unique_ptr<Acceptor>& acceptor) {
        int attempts = 0;
        while (true) {
            LOG_DEBUG("starting acceptor of {}", url_for(configured_port()));
            auto ec = try_bind(executor, endpoint, acceptor);
            if (!ec) return {};
            LOG_ERROR("cannot start listening on {}: {}", url_for(configured_port()), ec.message());
            if (options.max_listening_attempts >= 0 && ++attempts >= options.max_listening_attempts) return ec;
            std::unique_lock lock(mutex);
            if (changed.wait_for(lock, LISTEN_RETRY_DELAY, [this] { return stopping.load(); })) {
                return boost::asio::error::operation_aborted;
            }
        }
    }

    template<typename Endpoint, typename Acceptor>
    static boost::system::error_code try_bind(const boost::asio::any_io_executor& executor, const Endpoint& endpoint,
                                              std::unique_ptr<Acceptor>& result) {
        auto acceptor = std::make_unique<Acceptor>(executor);
        boost::system::error_code ec;
        acceptor->open(endpoint.protocol(), ec);
        if (!ec) close_on_exec(acceptor->native_handle());
        if constexpr (std::is_same_v<Acceptor, tcp::acceptor>) {
            if (!ec) acceptor->set_option(tcp::acceptor::reuse_address(true), ec);
        }
        if (!ec) acceptor->bind(endpoint, ec);
        if (!ec) acceptor->listen(boost::asio::socket_base::max_listen_connections, ec);
        if (!ec) result = std::move(acceptor);
        return ec;
    }

    // Make the acceptor (and its thread) the listener's, unless it stopped meanwhile
    template<typename Protocol>
    boost::system::error_code publish(std::unique_ptr<typename Protocol::acceptor> acceptor, acceptor_thread_ptr thread,
                                      tls_setup connections_tls) {
        uint16_t bound = 0;
        typename Protocol::endpoint local_endpoint;
        boost::system::error_code ec;
        local_endpoint = acceptor->local_endpoint(ec);
        if constexpr (std::is_same_v<Protocol, tcp>) {
            if (!ec) bound = local_endpoint.port();
        } else {
            set_permissions();
        }
        {
            std::scoped_lock lock(mutex);
            if (!stopping) {
                if constexpr (std::is_same_v<Protocol, tcp>) {
                    if (!ec) tcp_protocol = local_endpoint.protocol();
                }
                acceptor_of<Protocol>() = std::move(acceptor);
                acceptor_thread = std::move(thread);
                tls = std::move(connections_tls);
                port = bound;
                url = url_for(bound);
                opened = true;
            }
        }
        if (acceptor) {
            // Stopped while opening: its acceptor goes before its thread, and its socket file
            // with it (stop() removes it once open only)
            acceptor.reset();
            if constexpr (std::is_same_v<Protocol, local>) {
                std::error_code removed;
                std::filesystem::remove(std::get<unix_endpoint>(options.endpoint).path, removed);
            }
            return boost::asio::error::operation_aborted;
        }
        LOG_DEBUG("listening on {}", url_for(bound));
        return {};
    }

    // Let the owner and its group use the socket file
    void set_permissions() const {
        const auto& path = std::get<unix_endpoint>(options.endpoint).path;
        std::error_code ec;
        std::filesystem::permissions(path,
                                     std::filesystem::perms::owner_all |
                                     std::filesystem::perms::group_read |
                                     std::filesystem::perms::group_write,
                                     std::filesystem::perm_options::replace, ec);
        if (ec) LOG_WARNING("cannot set the permissions of Unix socket {}: {}", path, ec.message());
    }

    // --- start() ---

    void start(sync_callback<void(std::shared_ptr<socket>)> handler) {
        {
            std::scoped_lock lock(mutex);
            if (!opened || started || stopping || !handler) {
                LOG_ERROR("cannot start the listener on {}: {}", url_for(port),
                          !handler ? "no handler" : !opened ? "not open" : started ? "started already" : "stopped");
                return;
            }
            on_connection = std::move(handler);
            started = true;
            if (acceptor_thread) {
                if (is_tcp()) accept_on_thread<tcp>();
                else accept_on_thread<local>();
                return;
            }
        }
        if (is_tcp()) accept_next<tcp>();
        else accept_next<local>();
    }

    // With the mutex held: on a thread of its own, every pending connection is accepted at
    // once, without waiting
    template<typename Protocol>
    void accept_on_thread() {
        boost::system::error_code ec;
        acceptor_of<Protocol>()->non_blocking(true, ec);
        if (ec) {
            LOG_ERROR("cannot accept connections: {}", ec.message());
            return;
        }
        boost::asio::post(acceptor_thread->get_io_context(), [self = shared_from_this()] {
            self->accept_pending<Protocol>();
        });
    }

    // --- Accepting with executor providers ---

    // Without the mutex: accept the next connection on the next executor to serve on, unless
    // the listener stopped meanwhile (its acceptor is gone then)
    template<typename Protocol>
    void accept_next() {
        auto executor = serve_on();
        bool usable = io_context_of(executor) != nullptr;
        std::scoped_lock lock(mutex);
        if (stopping) return;
        auto& acceptor = acceptor_of<Protocol>();
        if (!usable) executor = acceptor->get_executor();
        acceptor->async_accept(executor, [self = shared_from_this(), usable]
            (const boost::system::error_code& e, typename Protocol::socket peer) {
                self->on_accepted<Protocol>(e, std::move(peer), usable);
            });
    }

    template<typename Protocol>
    void on_accepted(const boost::system::error_code& e, typename Protocol::socket peer, bool usable) {
        auto accepted_at = steady::now();
        {
            // Stopped meanwhile (from any thread): its acceptor is gone
            std::scoped_lock lock(mutex);
            if (stopping) return;
            if (e == boost::asio::error::operation_aborted) {
                LOG_DEBUG("stop accepting connections");
                return;
            }
            if (e) {
                LOG_ERROR("cannot accept more connections: {}", e.message());
                retry_accept<Protocol>();
                return;
            }
        }
        // set before it reaches the executor serving it
        close_on_exec(peer.native_handle());
        accept_next<Protocol>();
        if (!usable) {
            LOG_ERROR("cannot serve an accepted connection: the executor to serve on does not run on an io_context");
            return;
        }
        serve_without_throwing([&] {
            if (is_remote_allowed<Protocol>(peer.native_handle())) serve(std::move(peer), accepted_at);
        });
    }

    // --- Accepting on a thread of its own ---

    template<typename Protocol>
    void wait_connections() {
        acceptor_of<Protocol>()->async_wait(boost::asio::socket_base::wait_read,
            [self = shared_from_this()](const boost::system::error_code& e) {
                if (!e) {
                    self->accept_pending<Protocol>();
                } else if (e != boost::asio::error::operation_aborted) {
                    LOG_ERROR("cannot accept more connections: {}", e.message());
                    self->retry_accept<Protocol>();
                } else {
                    LOG_DEBUG("stop accepting connections");
                }
            });
    }

    // Only the acceptor thread uses the acceptor (stop() joins it before closing the acceptor),
    // and it is the only one running its io_context, so no other thread takes its readiness
    template<typename Protocol>
    void accept_pending() {
        auto& acceptor = acceptor_of<Protocol>();
        while (!stopping) {
#if defined(__linux__)
            int descriptor = ::accept4(acceptor->native_handle(), nullptr, nullptr, SOCK_CLOEXEC);
#else
            int descriptor = ::accept(acceptor->native_handle(), nullptr, nullptr);
            if (descriptor >= 0) close_on_exec(descriptor);
#endif
            if (descriptor >= 0) {
                accepted_descriptor accepted(descriptor);
                auto accepted_at = steady::now();
#if defined(SO_NOSIGPIPE)
                int enabled = 1;
                ::setsockopt(descriptor, SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled));
#endif
                serve_without_throwing([&] { hand_over<Protocol>(std::move(accepted), accepted_at); });
                continue;
            }
            int error = errno;
            if (error == EINTR || error == ECONNABORTED || error == EPROTO) continue;
            if (error == EAGAIN || error == EWOULDBLOCK) {
                wait_connections<Protocol>();
            } else {
                LOG_ERROR("cannot accept more connections: {}", std::error_code(error, std::system_category()).message());
                retry_accept<Protocol>();
            }
            return;
        }
    }

    // The connection is registered and set up on the executor serving it, waking it up once
    template<typename Protocol>
    void hand_over(accepted_descriptor descriptor, steady::time_point accepted_at) {
        if (!is_remote_allowed<Protocol>(descriptor.get())) return;
        auto executor = serve_on();
        if (!io_context_of(executor)) {
            LOG_ERROR("cannot serve an accepted connection: the executor to serve on does not run on an io_context");
            return;
        }
        auto protocol = protocol_of<Protocol>();
        boost::asio::post(executor,
            [self = shared_from_this(), executor, protocol, descriptor = std::move(descriptor), accepted_at]() mutable {
                typename Protocol::socket peer(executor);
                boost::system::error_code ec;
                peer.assign(protocol, descriptor.get(), ec);
                if (ec) {
                    LOG_ERROR("cannot serve an accepted connection: {}", ec.message());
                    return;
                }
                descriptor.release();
                serve_without_throwing([&] { self->serve(std::move(peer), accepted_at); });
            });
    }

    template<typename Protocol>
    Protocol protocol_of() const {
        if constexpr (std::is_same_v<Protocol, tcp>) return tcp_protocol;
        else return local();
    }

    // Accept again after a delay: an error accepting (e.g. out of descriptors) would repeat
    // at once otherwise
    template<typename Protocol>
    void retry_accept() {
        auto timer = std::make_shared<boost::asio::steady_timer>(acceptor_of<Protocol>()->get_executor(),
                                                                 ACCEPT_RETRY_DELAY);
        timer->async_wait([self = shared_from_this(), timer](const boost::system::error_code& e) {
            if (e) return;
            if (self->own_acceptor_thread) self->accept_pending<Protocol>();
            else self->accept_next<Protocol>();
        });
    }

    // Whether to serve a connection just accepted, for its remote address (TCP only)
    template<typename Protocol>
    bool is_remote_allowed(int descriptor) const {
        if constexpr (!std::is_same_v<Protocol, tcp>) {
            return true;
        } else {
            // The remote address is only looked up to filter it
            if (allowed_remotes.empty() && forbidden_remotes.empty()) return true;
            tcp::endpoint remote;
            auto size = static_cast<socklen_t>(remote.capacity());
            std::string remote_ip = "0.0.0.0";
            if (::getpeername(descriptor, remote.data(), &size) == 0) {
                remote.resize(size);
                remote_ip = remote.address().to_string();
            }
            if (!forbidden_remotes.contains(remote_ip) &&
                (allowed_remotes.empty() || allowed_remotes.contains(remote_ip))) {
                return true;
            }
            LOG_DEBUG("rejecting connection from: ip: {}, port: {}, secure: {}",
                        remote_ip, port.load(), secure);
            return false;
        }
    }

    // --- Serving ---

    // A connection accepted, on the executor serving it (see serve_without_throwing())
    void serve(tcp::socket peer, steady::time_point accepted_at) {
        auto sock = make_socket(std::move(peer));
        LOG_DEBUG("received connection from: ip: {}, port: {}, secure: {}",
                  sock->get_remote_ip(), sock->get_local_port(), sock->is_secure());
        // Fails if the connection is already gone: drop it
        if (options.tcp_no_delay && !sock->enable_tcp_no_delay()) return;
        if (sock->requires_handshake()) {
            handshake(std::move(sock), accepted_at);
        } else {
            deliver(std::move(sock));
        }
    }

    // A TLS connection is created with the context of the listener and asks for client
    // certificates with its policy (the mode of the host name, if it has one, replaces it in the
    // handshake: see certificate_manager::sni_callback())
    std::shared_ptr<tcp_socket> make_socket(tcp::socket peer) const {
        if (!tls.context) return std::make_shared<tcp_socket>("listener", std::move(peer));
        // On an io_context: checked as it was accepted
        auto* io_context = io_context_of(peer.get_executor());
        auto sock = std::make_shared<ssl_socket>("listener", *io_context, tls.context);
        sock->get_socket() = std::move(peer);
        sock->set_client_verify(tls.client_verify);
        return sock;
    }

    // --- TLS handshakes ---

    // The end of the handshake of a connection accepted at `accepted_at`: none without a limit
    std::optional<steady::time_point> handshake_deadline(steady::time_point accepted_at) const {
        if (options.handshake_timeout.count() <= 0) return std::nullopt;
        return accepted_at + options.handshake_timeout;
    }

    // Start the handshake of a TLS connection, registered as pending until it ends: on the
    // io_context serving it, or on the strand of its gate with a bound
    void handshake(std::shared_ptr<tcp_socket> sock, steady::time_point accepted_at) {
        pending_handshake entry{sock, sock->get_io_context().get_executor(), nullptr};
        {
            std::scoped_lock lock(mutex);
            if (stopping) {
                sock->close();
                return;
            }
            if (options.max_pending_handshakes > 0) {
                entry.gate = gate_of(sock->get_io_context());
                entry.executor = entry.gate->strand;
            }
            pending.emplace(sock.get(), entry);
        }
        co_spawn(entry.executor, serve_tls(shared_from_this(), std::move(sock), entry.gate, accepted_at),
                 log_serve_failure);
    }

    // With the mutex held
    std::shared_ptr<handshake_gate> gate_of(boost::asio::io_context& io_context) {
        auto& gate = gates[&io_context];
        if (!gate) gate = std::make_shared<handshake_gate>(io_context);
        return gate;
    }

    // The handshake of a TLS connection, then the handler, unless the listener stopped meanwhile
    static awaitable<void> serve_tls(std::shared_ptr<state> self, std::shared_ptr<tcp_socket> sock,
                                     std::shared_ptr<handshake_gate> gate, steady::time_point accepted_at) {
        auto deadline = self->handshake_deadline(accepted_at);
        boost::system::error_code ec;
        {
            pending_registration registered(self, sock.get());
            if (gate) {
                ec = co_await handshake_in_slot(self, sock, std::move(gate), deadline);
            } else {
                ec = co_await handshake_until(sock, deadline);
            }
        }
        // Not errors of the listener: any client (or scanner) may cause them
        if (ec == boost::asio::error::timed_out) {
            LOG_DEBUG("TLS handshake not done in time, remote ip: {}", sock->get_remote_ip());
            sock->close();
        } else if (ec && self->stopping) {
            sock->close();
        } else if (ec) {
            LOG_DEBUG("error while handling SSL handshake: {}, remote ip: {}", ec.message(), sock->get_remote_ip());
        } else {
            self->deliver(std::move(sock));
        }
    }

    // Removes a connection from the pending ones once its handshake ended, however it ended
    struct pending_registration {
        std::shared_ptr<state> self;
        const tcp_socket* sock;

        pending_registration(std::shared_ptr<state> listener_state, const tcp_socket* pending_sock)
            : self(std::move(listener_state)), sock(pending_sock) {}
        pending_registration(const pending_registration&) = delete;
        pending_registration& operator=(const pending_registration&) = delete;
        ~pending_registration() {
            std::scoped_lock lock(self->mutex);
            self->pending.erase(sock);
        }
    };

    // On the strand of the gate: the handshake in a slot, waited for within the deadline
    static awaitable<boost::system::error_code> handshake_in_slot(std::shared_ptr<state> self,
                                                                  std::shared_ptr<tcp_socket> sock,
                                                                  std::shared_ptr<handshake_gate> gate,
                                                                  std::optional<steady::time_point> deadline) {
        if (auto ec = co_await wait_for_slot(self, *gate, deadline)) co_return ec;
        handshake_slot slot(std::move(gate));
        co_return co_await handshake_until(std::move(sock), deadline);
    }

    // On the strand of the gate: until a slot is free (taken by the caller before it suspends
    // again), `timed_out` at the deadline, or `operation_aborted` once the listener stops
    static awaitable<boost::system::error_code> wait_for_slot(std::shared_ptr<state> self, handshake_gate& gate,
                                                              std::optional<steady::time_point> deadline) {
        while (true) {
            if (self->stopping) co_return boost::asio::error::operation_aborted;
            if (gate.in_progress < self->options.max_pending_handshakes) co_return boost::system::error_code{};
            // Set again by the next slot freed, or by stop()
            gate.slot_freed.reset();
            if (!deadline) {
                co_await gate.slot_freed.wait();
                continue;
            }
            auto left = *deadline - steady::now();
            if (left <= steady::duration::zero() || !co_await with_timeout(gate.slot_freed.wait(), left)) {
                co_return boost::asio::error::timed_out;
            }
        }
    }

    void serve(local::socket peer, steady::time_point) {
        auto sock = std::make_shared<unix_socket>("listener", std::move(peer));
        LOG_DEBUG("received connection on Unix socket: {}", std::get<unix_endpoint>(options.endpoint).path);
        deliver(std::move(sock));
    }

    // Hand a connection to the handler, unless the listener is stopping (see above)
    void deliver(std::shared_ptr<socket> sock) {
        auto delivering_thread = std::this_thread::get_id();
        {
            std::scoped_lock lock(mutex);
            if (stopping) {
                sock->close();
                return;
            }
            delivering.push_back(delivering_thread);
        }
        serve_without_throwing([&] { on_connection(std::move(sock)); });
        {
            // Not there if the handler stopped the listener
            std::scoped_lock lock(mutex);
            auto it = std::find(delivering.begin(), delivering.end(), delivering_thread);
            if (it != delivering.end()) delivering.erase(it);
        }
        changed.notify_all();
    }

    // --- stop() ---

    void stop() {
        std::unique_lock lock(mutex);
        bool first = !stopping;
        stopping = true;
        // A handler stopping the listener does not wait for itself
        std::erase(delivering, std::this_thread::get_id());
        if (first) {
            auto thread = std::move(acceptor_thread);
            auto handshakes = std::exchange(pending, {});
            // Their strands go with the handshakes still holding them, not with this state
            auto released_gates = std::exchange(gates, {});
            auto released_manager = std::exchange(certificates, nullptr);
            lock.unlock();
            changed.notify_all();   // an open() waiting to bind again gives up
            close(std::move(thread));
            end_handshakes(handshakes);
            released_gates.clear();
            release_session_cache();
            // Destroyed here if nothing else holds it, outside the lock: the handshakes still
            // running find it gone (see certificate_manager::sni_callback())
            released_manager.reset();
            lock.lock();
        }
        changed.wait(lock, [this] { return delivering.empty(); });
    }

    // Close the connections whose handshake is pending, on the executor they handshake on, and
    // wake those waiting for a slot: they see the listener stopping and end, without serving.
    // Those gone already (their io_context with them, maybe) are left alone
    static void end_handshakes(const std::unordered_map<const tcp_socket*, pending_handshake>& handshakes) {
        for (const auto& [_, handshake] : handshakes) {
            if (handshake.sock.expired()) continue;
            boost::asio::post(handshake.executor, [weak = handshake.sock, gate = handshake.gate] {
                if (auto sock = weak.lock()) sock->close();
                if (gate) gate->slot_freed.set();
            });
        }
    }

    // Close the acceptor, once its thread (if any) is not using it, and the Unix socket file.
    // The accept handlers still to run do nothing, as the listener is stopping
    void close(acceptor_thread_ptr thread) {
        if (thread) thread->stop();
        std::unique_ptr<tcp::acceptor> tcp_closed;
        std::unique_ptr<local::acceptor> unix_closed;
        bool was_open = false;
        {
            std::scoped_lock lock(mutex);
            tcp_closed = std::move(tcp_acceptor);
            unix_closed = std::move(unix_acceptor);
            was_open = opened;
        }
        close_acceptor(tcp_closed);
        close_acceptor(unix_closed);
        port = 0;
        if (const auto* unix = std::get_if<unix_endpoint>(&options.endpoint); unix && was_open) {
            std::error_code ec;
            std::filesystem::remove(unix->path, ec);
            if (ec) LOG_WARNING("cannot remove Unix socket file {}: {}", unix->path, ec.message());
        }
        // The thread goes after its acceptor, with the handlers it never ran
        tcp_closed.reset();
        unix_closed.reset();
    }

    template<typename Acceptor>
    static void close_acceptor(const std::unique_ptr<Acceptor>& acceptor) {
        if (!acceptor || !acceptor->is_open()) return;
        boost::system::error_code ec;
        acceptor->close(ec);
        if (ec) LOG_WARNING("cannot close the acceptor: {}", ec.message());
    }
};

listener::listener(listener_options options, executor_provider accept_on, executor_provider serve_on)
    : state_(std::make_shared<state>(std::move(options), std::move(accept_on), std::move(serve_on))) {}

listener::~listener() {
    try {
        stop();
    } catch (const std::exception& e) {
        LOG_ERROR("cannot stop a listener: {}", e.what());
    }
}

boost::system::error_code listener::open() {
    return state_->open();
}

void listener::start(sync_callback<void(std::shared_ptr<socket>)> on_connection) {
    state_->start(std::move(on_connection));
}

void listener::stop() {
    state_->stop();
}

uint16_t listener::local_port() const {
    return state_->port;
}

std::string listener::url() const {
    std::scoped_lock lock(state_->mutex);
    return state_->url;
}

void detail::listener_access::use_acceptor_thread(listener& l) {
    std::scoped_lock lock(l.state_->mutex);
    if (!l.state_->opened) l.state_->own_acceptor_thread = true;
}

} // namespace thinger::asio

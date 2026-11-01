#ifndef THINGER_HTTP_POOL_SERVER_HPP
#define THINGER_HTTP_POOL_SERVER_HPP

#include <condition_variable>
#include <mutex>
#include "server_base.hpp"
#include "../../asio/workers.hpp"

namespace thinger::http {

/**
 * HTTP server served on a pool of workers: it accepts on a thread of its own and serves each
 * connection on the next thread of the pool. The application owns the workers, starts them,
 * and keeps them alive longer than the server; several servers (and clients) may share them.
 */
class pool_server : public server_base {
protected:
    // A listener accepting on a thread of its own and serving on the workers
    std::unique_ptr<asio::listener> create_listener(asio::listener_options options) override;

    boost::asio::io_context* shutdown_context() override;
    bool in_server_thread() override;
    
public:
    explicit pool_server(asio::workers& workers);
    ~pool_server() override;

    // Stop as server_base::stop() does (its connections are closed on the workers), and
    // release wait()
    void stop() override;
    // Wait until the server stops (stop(), or the end of a shutdown)
    void wait() override;

private:
    // wait() returns
    void release_wait();

    asio::workers& workers_;
    std::mutex wait_mutex_;
    std::condition_variable wait_cv_;
    bool stopped_ = false;
};

} // namespace thinger::http

#endif // THINGER_HTTP_POOL_SERVER_HPP

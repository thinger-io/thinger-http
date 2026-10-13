#ifndef THINGER_ASIO_WORKERS
#define THINGER_ASIO_WORKERS

#include <atomic>
#include <chrono>
#include <optional>
#include <unordered_map>
#include <set>
#include <boost/asio.hpp>
#include "worker_thread.hpp"
#include "worker_client.hpp"

namespace thinger::asio {

	class workers {
	public:
        workers();
		virtual ~workers();

		/// start the asio workers
        bool start(size_t working_threads=std::thread::hardware_concurrency());

        /// stop the asio workers and all their pending async operations
        bool stop();

        /// keep the asio workers alive until a signal is received; then stop them (and their
        /// clients, as the pool servers) at once
        void wait(const std::set<unsigned>& signals = {SIGINT, SIGTERM, SIGQUIT});

        /// the same, stopping gracefully on the first signal: the clients finish what they
        /// serve within `grace` (a pool_server shuts down, see http_server_base::shutdown()),
        /// then the workers stop. A second signal meanwhile stops them at once.
        void wait_graceful(std::chrono::milliseconds grace, const std::set<unsigned>& signals = {SIGINT, SIGTERM, SIGQUIT});
        
        /// check if workers are running
        bool running() const { return running_; }

        /// return an isolated io_context not shared in the pool
        boost::asio::io_context& get_isolated_io_context(std::string thread_name);

        /// return the next io_context (round-robin); can be called from any thread
        boost::asio::io_context& get_next_io_context();

        /// return the io_context associated with the caller thread
		boost::asio::io_context& get_thread_io_context();

        /// whether the caller thread is one of the workers (of the pool, or isolated)
        bool in_worker_thread();

        // Client management
        /// Register a client that uses workers
        void register_client(worker_client* client);
        
        /// Unregister a client
        void unregister_client(worker_client* client);
        
        /// Get number of registered clients
        size_t client_count() const;
        
        /// Enable/disable automatic management based on clients
        void set_auto_manage(bool enable) { auto_manage_ = enable; }
        
        /// Check if auto management is enabled
        bool is_auto_managed() const { return auto_manage_; }

	private:
        /// Internal method to perform the actual stop
        void do_stop();

        /// wait for the signals, then stop at once (no grace) or gracefully
        void wait_for_signals(const std::set<unsigned>& signals, std::optional<std::chrono::milliseconds> grace);

        /// ask every client to stop gracefully, and stop once all of them did
        void begin_graceful_stop(std::chrono::milliseconds grace);

        /// the graceful stop in progress, if any (on the wait context)
        struct graceful_stop_state;
        std::shared_ptr<graceful_stop_state> graceful_stop_;
        
        /// mutex used for initializing threads and data structures
        std::mutex mutex_;

        // io_context used for capturing signals and wait coordination
        boost::asio::io_context wait_context_;
        boost::asio::signal_set signals_;
        
        // Work guard to keep wait_context_ running
        using work_guard_type = boost::asio::executor_work_guard<boost::asio::io_context::executor_type>;
        std::unique_ptr<work_guard_type> wait_work_;

        /// flag for controlling if it is running
		std::atomic<bool> running_{false};

        /// index to control next io_context to use (in the round-robin): taken by any
        /// thread accepting connections, so atomic (relaxed: it only spreads the load)
        std::atomic<unsigned> next_io_context_{0};

		/// worker threads used for general asio pool
		std::vector<std::unique_ptr<worker_thread>> worker_threads_;

        /// worker threads allocated from isolated_io_contexts
        std::vector<std::unique_ptr<worker_thread>> job_threads_;

		/// relation of all worker threads with their worker thread instance
        std::unordered_map<std::thread::id, std::reference_wrapper<worker_thread>> workers_threads_map_;

        // Client management
        /// Set of registered clients
        std::set<worker_client*> clients_;
        
        /// Mutex for client management
        mutable std::mutex clients_mutex_;
        
        /// Flag to enable/disable automatic management
        std::atomic<bool> auto_manage_{true};

	};

	// Singleton instance accessor
	workers& get_workers();

}

#endif

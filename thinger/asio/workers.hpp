#ifndef THINGER_ASIO_WORKERS
#define THINGER_ASIO_WORKERS

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <boost/asio/any_io_executor.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/system/error_code.hpp>
#include "worker_thread.hpp"

namespace thinger::asio {

	// A pool of threads, each running an io_context of its own, plus the isolated threads asked
	// for. An ordinary object: the application owns it, starts it and stops it (nothing starts
	// or stops it by itself), and hands it by reference to the servers and clients that run on
	// it (pool_server, async_client). They own no thread, and the application keeps the workers
	// alive longer than them. Several instances may live in one process; they share nothing.
	//
	// Never destroy a workers from one of its own threads.
	class workers {
	public:
        workers();
		~workers();

        workers(const workers&) = delete;
        workers& operator=(const workers&) = delete;

		/// start `threads` threads in the pool: `already_started` if it runs already (no thread
		/// is added), `invalid_argument` for 0 threads. Their io_contexts are kept until the
		/// workers are destroyed, also when they start again after stop()
        boost::system::error_code start(size_t threads = std::thread::hardware_concurrency());

        /// stop every thread, the pool's and the isolated ones, destroying their pending
        /// operations, and release the wait() calls in progress. From any thread, more than once
        /// (a warning once stopped). Stop the servers and clients on it first. Their io_contexts
        /// live on, stopped, until the workers are destroyed: a client destroyed afterwards
        /// still closes its connections on them. Called from one of its threads, that thread
        /// ends once its handler returns, and the destructor joins it.
        void stop();

        /// whether the workers run
        bool running() const { return running_; }

        /// the executor of the next thread of the pool (round-robin), from any thread, without
        /// a lock; a null executor when the workers do not run
        boost::asio::any_io_executor next_executor();

        /// next_executor(), as a callable (for a listener)
        std::function<boost::asio::any_io_executor()> provider();

        /// the executor of a new thread of its own, out of the pool, named `name`: it runs until
        /// the workers stop. A null executor when the workers do not run
        boost::asio::any_io_executor isolated_executor(std::string name);

        /// whether the caller thread is one of the threads these workers started (of the pool
        /// or isolated) and they still run: a thread-local, without a lock
        bool in_worker_thread() const;

        /// block until one of `signals` arrives, and return it; 0 when stop() released it
        /// instead, or at once if the workers do not run. The workers keep running: what to do
        /// with the signal (a server's shutdown(), then stop()) is the application's. With an
        /// empty set, it waits for stop(). While waiting, the signals do not take their default
        /// action; afterwards they do again.
        int wait(std::set<int> signals = {SIGINT, SIGTERM, SIGQUIT});

        /// the io_context of the next thread of the pool (round-robin), without a lock. Throws
        /// std::logic_error when the workers do not run
        boost::asio::io_context& get_next_io_context();

        /// the io_context of the caller thread when it is one of these workers' threads (from a
        /// thread-local, without a lock), or else the one of the first thread of the pool, which
        /// throws std::logic_error when the workers do not run. It goes at 0.10
		boost::asio::io_context& get_thread_io_context();

	private:
        // The pool's io_contexts, published by start() and read without a lock: immutable, and
        // kept (as the io_contexts) until the workers are destroyed
        struct pool_table {
            std::vector<boost::asio::io_context*> contexts;
        };

        /// the io_context of the next thread of the pool, or null when not running
        boost::asio::io_context* next_context();

        /// start a thread of these workers, marked as theirs
        std::unique_ptr<worker_thread> start_thread(std::string name);

        /// guards start(), stop(), the threads and the waits
        mutable std::mutex mutex_;

        /// the stop() calls still joining threads, which the destructor waits for (one called
        /// from a thread of the workers returns after the wait()s it releases)
        size_t stopping_ = 0;
        std::condition_variable stopped_;

        /// flag for controlling if it is running
		std::atomic<bool> running_{false};

        /// the published pool table (null when not running), and every table published
        std::atomic<const pool_table*> table_{nullptr};
        std::vector<std::unique_ptr<const pool_table>> tables_;

        /// index of the next io_context of the round-robin (relaxed: it only spreads the load)
        std::atomic<unsigned> next_io_context_{0};

		/// threads running: of the pool, and the isolated ones
		std::vector<std::unique_ptr<worker_thread>> worker_threads_;
        std::vector<std::unique_ptr<worker_thread>> isolated_threads_;

        /// threads stopped, with their io_contexts (see stop())
        std::vector<std::unique_ptr<worker_thread>> stopped_threads_;

        /// the io_contexts of the wait() calls in progress, which stop() releases
        std::vector<boost::asio::io_context*> waits_;
	};

}

#endif

#include "workers.hpp"
#include <algorithm>
#include <stdexcept>
#include <pthread.h>
#include <boost/asio/error.hpp>
#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/post.hpp>
#include <boost/asio/signal_set.hpp>
#include "../util/logger.hpp"

namespace thinger::asio{

    namespace {

    // The workers that started the caller thread, and its io_context: set by the thread itself,
    // before any other work, so it is read without a lock
    struct thread_mark {
        const workers* owner = nullptr;
        boost::asio::io_context* context = nullptr;
    };

    thread_mark& this_thread_mark() {
        static thread_local thread_mark mark;
        return mark;
    }

    // The name of the caller thread, as the system shows it (truncated to its 15 characters)
    void name_this_thread(const std::string& name) {
        auto truncated = name.substr(0, 15);
#if defined(__APPLE__)
        pthread_setname_np(truncated.c_str());
#elif defined(__linux__)
        pthread_setname_np(pthread_self(), truncated.c_str());
#endif
    }

    }

    workers::workers() = default;

    workers::~workers()
    {
        if(running_) {
            LOG_WARNING("workers destructor called while still running - forcing stop");
            stop();
        }
        // A stop() called from one of its threads may still be joining the others
        {
            std::unique_lock<std::mutex> lock(mutex_);
            stopped_.wait(lock, [this] { return stopping_ == 0; });
        }
        // Those that called stop() themselves, then the io_contexts with what they left pending
        for(auto const& thread : stopped_threads_){
            thread->stop();
        }
        stopped_threads_.clear();
    }

    boost::system::error_code workers::start(size_t threads)
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        if(running_) return boost::asio::error::already_started;
        if(threads == 0) return make_error_code(boost::system::errc::invalid_argument);

        LOG_DEBUG("starting {} working threads in the shared pool", threads);
        auto table = std::make_unique<pool_table>();
        worker_threads_.reserve(threads);
        for(size_t thread_number=1; thread_number<=threads; ++thread_number){
            auto worker = start_thread("worker thread " + std::to_string(thread_number));
            table->contexts.push_back(&worker->get_io_context());
            worker_threads_.emplace_back(std::move(worker));
        }

        next_io_context_.store(0, std::memory_order_relaxed);
        table_.store(table.get());
        tables_.emplace_back(std::move(table));
        running_ = true;
        return {};
    }

    std::unique_ptr<worker_thread> workers::start_thread(std::string name)
    {
        auto worker = std::make_unique<worker_thread>(name);
        auto& context = worker->get_io_context();
        // The first work of the thread: it marks itself as one of these workers
        boost::asio::post(context, [this, &context, name = std::move(name)] {
            this_thread_mark() = {this, &context};
            name_this_thread(name);
        });
        worker->start();
        return worker;
    }

    boost::asio::any_io_executor workers::isolated_executor(std::string name)
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        if(!running_) return {};
        LOG_DEBUG("starting '{}' worker thread", name);
        auto worker = start_thread(std::move(name));
        auto executor = worker->get_io_context().get_executor();
        isolated_threads_.emplace_back(std::move(worker));
        return executor;
    }

    void workers::stop()
    {
        std::vector<std::unique_ptr<worker_thread>> threads;
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if(!running_) {
                LOG_WARNING("workers already stopped");
                return;
            }
            running_ = false;
            ++stopping_;
            table_.store(nullptr);
            for(auto* wait : waits_) wait->stop();
            threads = std::move(isolated_threads_);
            isolated_threads_.clear();
            std::move(worker_threads_.begin(), worker_threads_.end(), std::back_inserter(threads));
            worker_threads_.clear();
        }

        // Out of the lock, as a handler finishing meanwhile may call back into the workers:
        // every io_context stops at once, then each thread is joined, but the caller's own
        LOG_DEBUG("stopping {} worker threads", threads.size());
        for(auto const& thread : threads){
            thread->get_io_context().stop();
        }
        // Then the pending handlers of each are destroyed; the io_contexts are kept (see stop()
        // in workers.hpp)
        auto* own_context = this_thread_mark().owner == this ? this_thread_mark().context : nullptr;
        for(auto const& thread : threads){
            if(&thread->get_io_context() == own_context) continue;
            thread->stop();
            thread->shutdown();
        }

        std::scoped_lock<std::mutex> lock(mutex_);
        std::move(threads.begin(), threads.end(), std::back_inserter(stopped_threads_));
        --stopping_;
        stopped_.notify_all();
        LOG_DEBUG("all done!");
    }

    int workers::wait(std::set<int> signals)
    {
        boost::asio::io_context context{1};
        // With no signal, it waits for stop()
        auto work = boost::asio::make_work_guard(context);
        boost::asio::signal_set signal_set(context);
        for(auto signal : signals){
            signal_set.add(signal);
        }

        int received = 0;
        signal_set.async_wait([&](const boost::system::error_code& ec, int signal_number){
            if(ec) return;
            LOG_INFO("received signal {}", signal_number);
            received = signal_number;
            context.stop();
        });

        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if(!running_) return 0;
            waits_.push_back(&context);
        }
        context.run();
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            std::erase(waits_, &context);
        }
        return received;
    }

    boost::asio::io_context* workers::next_context()
    {
        auto* table = table_.load();
        if(!table) return nullptr;
        auto next = next_io_context_.fetch_add(1, std::memory_order_relaxed);
        return table->contexts[next % table->contexts.size()];
    }

    boost::asio::any_io_executor workers::next_executor()
    {
        auto* context = next_context();
        if(!context) return {};
        return context->get_executor();
    }

    std::function<boost::asio::any_io_executor()> workers::provider()
    {
        return [this] { return next_executor(); };
    }

	boost::asio::io_context& workers::get_next_io_context()
	{
        auto* context = next_context();
        if(!context) throw std::logic_error("thinger::asio::workers: not running");
        return *context;
	}

	boost::asio::io_context& workers::get_thread_io_context()
	{
        // One of its threads: its io_context lives as long as the workers
        const auto& mark = this_thread_mark();
        if(mark.owner == this) return *mark.context;

        // Any other thread: the first thread of the pool
        auto* table = table_.load();
        if(!table) throw std::logic_error("thinger::asio::workers: not running");
        return *table->contexts.front();
	}

    bool workers::in_worker_thread() const
    {
        return this_thread_mark().owner == this && running_;
    }

}

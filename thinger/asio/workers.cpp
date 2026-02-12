#include "workers.hpp"
#include <boost/bind/bind.hpp>
#include <boost/asio/post.hpp>
#include <mutex>
#include <algorithm>
#include "../util/logger.hpp"

namespace thinger::asio{

	// Lazy singleton implementation
	workers& get_workers() {
		static workers instance;
		return instance;
	}

	using std::thread;
	using std::bind;
	using std::shared_ptr;
	using std::make_shared;

    workers::workers() :
        signals_(wait_context_){

    }

    workers::~workers()
    {
        if(running_) {
            LOG_WARNING("workers destructor called while still running - forcing stop");
            stop();
        }
    }

    void workers::wait(const std::set<unsigned>& signals) {
        LOG_DEBUG("registering stop signals...");
        
        // Keep the wait_context_ running
        wait_work_ = std::make_unique<work_guard_type>(wait_context_.get_executor());
        
        for (auto signal: signals){
            signals_.add(signal);
        }

        // wait for any signal to be received
        signals_.async_wait([this](const boost::system::error_code& ec, int signal_number){
            if(!ec){
                LOG_INFO("received signal: {}", signal_number);
                stop();
            }
        });

        // Run until stop() is called
        wait_context_.run();
        
        // Clean up
        wait_work_.reset();
    }


    bool workers::start(size_t worker_threads)
    {
        std::scoped_lock<std::mutex> lock(mutex_);
        if(running_) return false;
        running_ = true;

        LOG_INFO("starting {} working threads in the shared pool", worker_threads);
        worker_threads_.reserve(worker_threads);
        for(auto thread_number=1; thread_number<=worker_threads; ++thread_number){
            auto worker = std::make_unique<worker_thread>("worker thread " + std::to_string(thread_number));
            auto id = worker->start();
            workers_threads_map_.emplace(id, *worker);
            worker_threads_.emplace_back(std::move(worker));
        }

        return running_;
    }

    boost::asio::io_context& workers::get_isolated_io_context(std::string thread_name)
    {
        LOG_INFO("starting '{}' worker thread", thread_name);
        auto worker = std::make_unique<worker_thread>(std::move(thread_name));
        auto& io_context = worker->get_io_context();
        auto thread_id = worker->start();
        auto& worker_ref = *worker;
        job_threads_.emplace_back(std::move(worker));
        workers_threads_map_.emplace(thread_id, worker_ref);
        return io_context;
    }

    void workers::do_stop()
    {
        LOG_INFO("executing full stop");
        
        // First, notify all clients to stop
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            if (!clients_.empty()) {
                LOG_INFO("Stopping {} worker clients", clients_.size());
                for (auto* client : clients_) {
                    if (client && client->is_running()) {
                        try {
                            LOG_DEBUG("Stopping client: {}", client->get_service_name());
                            client->stop();
                        } catch (const std::exception& e) {
                            LOG_ERROR("Error stopping client {}: {}", client->get_service_name(), e.what());
                        }
                    }
                }
            }
        }
        
        // Stop all job threads
        LOG_INFO("stopping job threads");
        for(auto const& worker_thread : job_threads_){
            worker_thread->stop();
        }

        // Stop all worker threads
        LOG_INFO("stopping worker threads");
        for(auto const& worker_thread : worker_threads_){
            worker_thread->stop();
        }

        // Clear auxiliary references
        LOG_INFO("clearing structures");
        worker_threads_.clear();
        job_threads_.clear();
        workers_threads_map_.clear();
        next_io_context_ = 0;

        // Cancel signals and stop wait_context
        LOG_INFO("stopping wait context");
        signals_.cancel();
        wait_work_.reset();
        wait_context_.stop();
        
        LOG_INFO("all done!");
    }

    bool workers::stop()
    {
        //LOG_INFO("workers::stop() called");
        
        // Check if already stopping
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            if(!running_) {
                LOG_WARNING("workers already stopped");
                return false;
            }
            running_ = false;
        }

        // Check if wait_context_ is running in the current thread
        if (wait_context_.get_executor().running_in_this_thread()) {
            // We're already in wait_context_, execute directly
            //LOG_INFO("executing stop directly in wait_context thread");
            do_stop();
        } else if (wait_work_) {
            // wait_context_ is running in another thread, post to it
            //LOG_INFO("posting stop to wait_context thread");
            boost::asio::post(wait_context_, [this]() {
                do_stop();
            });
        } else {
            // wait_context_ is not running, execute directly
            //LOG_INFO("wait_context not running, executing stop directly");
            do_stop();
        }

        return true;
    }

	boost::asio::io_context& workers::get_next_io_context()
	{
        return worker_threads_[next_io_context_++%worker_threads_.size()]->get_io_context();
	}

	boost::asio::io_context& workers::get_thread_io_context()
	{
        std::thread::id this_id = std::this_thread::get_id();
        
        // First check if this is a worker thread
        {
            std::scoped_lock<std::mutex> lock(mutex_);
            auto it = workers_threads_map_.find(this_id);
            if(it != workers_threads_map_.end()){
                return it->second.get().get_io_context();
            }
        }
        
        // Not a worker thread - return a worker thread's io_context
        // The actual thread affinity will be handled differently
        LOG_DEBUG("Thread is not a worker thread, using first worker's io_context");
        return worker_threads_.begin()->get()->get_io_context();
	}

    // Client management implementation
    void workers::register_client(worker_client* client) {
        if (!client) return;
        
        bool should_start = false;
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            auto result = clients_.insert(client);
            
            if (result.second) { // Successfully inserted
                LOG_INFO("Worker client registered: {}", client->get_service_name());
                
                // Auto-start if this is the first client and auto-manage is enabled
                if (auto_manage_ && clients_.size() == 1 && !running_) {
                    LOG_INFO("First worker client registered, starting workers automatically");
                    should_start = true;
                }
            }
        }
        
        // Start outside the lock to avoid potential deadlock
        if (should_start) {
            start();
        }
    }
    
    void workers::unregister_client(worker_client* client) {
        if (!client) return;

        bool should_stop = false;
        
        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            size_t removed = clients_.erase(client);
            
            if (removed > 0) {
                LOG_INFO("Worker client unregistered: {}", client->get_service_name());
                
                // Auto-stop if no clients remain and auto-manage is enabled
                if (auto_manage_ && clients_.empty() && running_) {
                    LOG_INFO("Last worker client unregistered, stopping workers automatically");
                    should_stop = true;
                }
            }
        }
        
        // Stop outside the lock to avoid potential deadlock
        if (should_stop) {
            stop();
        }
    }
    
    size_t workers::client_count() const {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        return clients_.size();
    }

}
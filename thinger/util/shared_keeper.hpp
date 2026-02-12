#ifndef THINGER_UTIL_SHARED_KEEPER_HPP
#define THINGER_UTIL_SHARED_KEEPER_HPP

#include <boost/asio.hpp>
#include <memory>
#include <utility>
#include "logger.hpp"

namespace thinger::util{

    template<class T>
    class shared_keeper : public std::enable_shared_from_this<shared_keeper<T>>{
    public:

        shared_keeper(boost::asio::io_context& io_context) :
            timer_(io_context)
        {

        }

        virtual ~shared_keeper(){
            timer_.cancel();
        }

        void keep(std::shared_ptr<T> keep_alive_instance, std::function<void()> timeout_handler, std::chrono::seconds seconds = std::chrono::seconds{10}){
            clear();
            shared_instance_ = keep_alive_instance;
            timeout_handler_ = std::move(timeout_handler);
            timeout_seconds_ = seconds;
            elapsed_seconds_ = std::chrono::seconds{0};
            check_timeout();
        }

        void clear(){
            timer_.cancel();
            shared_instance_.reset();
        }

        void heartbeat(){
            idle_ = false;
        }

        [[nodiscard]] std::chrono::seconds timeout() const{
            return timeout_seconds_;
        }

        bool timed_out() const{
            return timed_out_;
        }

        void update_interval(std::chrono::seconds interval){
            timeout_seconds_ = interval;
            // cancel current timer
            timer_.cancel();
            // re-schedule timeout
            check_timeout();
        }

    private:

        void check_timeout()
        {
            idle_ = true;
            timed_out_ = false;
            
            // Adaptive check interval based on timeout duration
            std::chrono::seconds check_interval;
            
            if (timeout_seconds_ <= std::chrono::seconds{10}) {
                // For short timeouts (≤ 10s), check every second for precision
                check_interval = std::chrono::seconds{1};
            } else if (timeout_seconds_ <= std::chrono::seconds{60}) {
                // For medium timeouts (≤ 60s), check every 5 seconds
                check_interval = std::chrono::seconds{5};
            } else {
                // For long timeouts (> 60s), check every 10% of timeout (min 10s, max 30s)
                auto tenth = timeout_seconds_ / 10;
                check_interval = std::min(std::max(tenth, std::chrono::seconds{10}), std::chrono::seconds{30});
            }
            
            auto remaining = timeout_seconds_ - elapsed_seconds_;
            auto interval_to_use = std::min(check_interval, remaining);
            
            timer_.expires_from_now(boost::posix_time::seconds(interval_to_use.count()));
            timer_.async_wait(
                [this, self = std::enable_shared_from_this<shared_keeper<T>>::shared_from_this(), check_interval](const boost::system::error_code& e){

                    // if timer was not cancelled (just expired by itself)
                    if(e != boost::asio::error::operation_aborted){
                        if(!idle_){
                            // Had activity, reset the elapsed time
                            elapsed_seconds_ = std::chrono::seconds{0};
                            check_timeout();
                        }else{
                            // No activity, increment elapsed time by actual interval used
                            auto remaining_before = timeout_seconds_ - elapsed_seconds_;
                            auto actual_interval = std::min(check_interval, remaining_before);
                            elapsed_seconds_ += actual_interval;
                            
                            if(elapsed_seconds_ >= timeout_seconds_){
                                // Timeout reached
                                timed_out_ = true;
                                if(shared_instance_){
                                    timeout_handler_();
                                    shared_instance_.reset();
                                }
                            }else{
                                // Continue checking
                                check_timeout();
                            }
                        }
                    }else{
                        LOG_TRACE("shared_keeper cancelled: %s", e.message().c_str());
                        shared_instance_.reset();
                    }
                }
            );
        }

        boost::asio::deadline_timer timer_;
        std::shared_ptr<T> shared_instance_;
        std::function<void()> timeout_handler_;
        std::chrono::seconds timeout_seconds_{0};
        std::chrono::seconds elapsed_seconds_{0};
        bool idle_ = false;
        bool timed_out_ = false;
    };
}

#endif
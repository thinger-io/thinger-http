#ifndef THINGER_CHUNKED_PIPE_HPP
#define THINGER_CHUNKED_PIPE_HPP

#include <memory>
#include <functional>
#include "../../util/logger.hpp"
#include "request.hpp"
#include "../../asio/sockets/socket.hpp"

namespace thinger::http
{
    class chunked_pipe : public std::enable_shared_from_this<chunked_pipe>{
    public:

        static const int MAX_BUFFER_SIZE = 1024;

        chunked_pipe(std::shared_ptr<base::socket> source,  std::shared_ptr<request> target) : source_(source), target_(target), running_(false) {
            LOG_F(2, "<chunked pipe> created");
        }

        virtual ~chunked_pipe(){
            LOG_F(2, "<chunked pipe> released");
            if(on_end_) on_end_();
        }

        void start(){
            if(!running_){
                running_=true;
                read_source();
            }
        }

        void set_on_end_listener(std::function<void()> listener){
            on_end_ = listener;
        }

        void cancel(){
            if(source_->is_open()) source_->cancel();
            target_->end_chunk_response();
        }

        std::shared_ptr<base::socket> get_source_socket(){
            return source_;
        }

        std::shared_ptr<base::socket> get_target_socket(){
            return nullptr;
        }

    private:
        void source_error(const std::string& action, const boost::system::error_code& e){
            LOG_F(2, "source stopped on %s: %s", action.c_str(), e.message().c_str());
            cancel();
        }

        void target_error(const std::string& action, const boost::system::error_code& e){
            LOG_F(2, "target stopped on %s: %s", action.c_str(), e.message().c_str());
            cancel();
        }

        void read_source(){
            source_->async_read_some((uint8_t*)source_buffer_, MAX_BUFFER_SIZE, [this, self = shared_from_this()](const boost::system::error_code& e, std::size_t bytes_transferred){
                if(e) return source_error("read", e);
                target_->write_chunk_response(std::string((char*) source_buffer_, bytes_transferred));
                if(target_->get_http_connection()->get_socket()->is_open()){
                    read_source();
                }
            });
        }

    private:
        std::shared_ptr<base::socket> source_;
        std::shared_ptr<request> target_;
        std::function<void()> on_end_;
        char source_buffer_[MAX_BUFFER_SIZE];
        bool running_;
    };
}

#endif
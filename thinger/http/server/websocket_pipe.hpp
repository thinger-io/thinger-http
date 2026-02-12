
#ifndef THINGER_WEBSOCKET_PIPE_HPP
#define THINGER_WEBSOCKET_PIPE_HPP

#include "websocket_connection.hpp"

namespace thinger::server::base{

    using namespace thinger::server::http;

    class websocket_pipe : public std::enable_shared_from_this<websocket_pipe>{
    public:

        static const int MAX_BUFFER_SIZE = 1024;

        websocket_pipe(std::shared_ptr<websocket_connection> source, std::shared_ptr<base::socket> target) : source_(source), target_(target), running_(false) {
            LOG_F(2, "<websocket pipe> created");
        }

        virtual ~websocket_pipe(){
            LOG_F(2, "<websocket pipe> released");
        }

        void start(){
            if(!running_){
                if(auto source = source_.lock()){
                    running_=true;
                    source->on_message([this, self = shared_from_this()](std::string frame, bool binary) {
                        LOG_F(3, "received ws frame");
                        auto frame_str = std::make_shared<std::string>(std::move(frame));
                        target_->async_write((uint8_t *) frame_str->data(), frame_str->size(),
                                             [this, frame, self = shared_from_this()](
                                                     const boost::system::error_code &e,
                                                     std::size_t bytes_transferred) {
                                                 if (e) return target_error("write", e);
                                                 LOG_F(3, "wrote %zu bytes to target", bytes_transferred);
                                             });
                    });
                    read_target();
                }
            }
        }

        void cancel(){
            if(auto source = source_.lock()) source->stop();
            if(target_) target_->cancel();
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

        void read_target(){
            target_->async_read_some((uint8_t*)target_buffer_, MAX_BUFFER_SIZE, [this, self = shared_from_this()](const boost::system::error_code& e, std::size_t bytes_transferred){
                if(e) return target_error("read", e);
                LOG_F(3, "read %zu bytes from target", bytes_transferred);
                if(auto source = source_.lock()){
                    source->send_binary(std::string(target_buffer_, bytes_transferred));
                    LOG_F(3, "wrote %zu bytes to source", bytes_transferred);
                    read_target();
                }
            });
        }

    private:
        std::weak_ptr<websocket_connection> source_;
        std::shared_ptr<base::socket> target_;
        char target_buffer_[MAX_BUFFER_SIZE];
        bool running_;
    };
}

#endif
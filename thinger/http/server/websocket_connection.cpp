#include "../../util/logger.hpp"
#include "../../util/hex.hpp"
#include "../../util/types.hpp"
#include "websocket_connection.hpp"
#include <boost/iostreams/device/file.hpp>
#include <boost/iostreams/stream.hpp>
#include <utility>
#include "../util/utf8.hpp"

namespace thinger::http{

    std::atomic<unsigned long> websocket_connection::connections(0);

    websocket_connection::websocket_connection(std::shared_ptr<asio::websocket> socket) :
        ws_(std::move(socket))
    {
        connections++;
        LOG_LEVEL(2, "websocket connection created. current: {}", (unsigned long) connections);

    }

    websocket_connection::~websocket_connection()
    {
        connections--;
        LOG_LEVEL(1, "releasing websocket connection. current: {}", (unsigned long) connections);
    }

    void websocket_connection::on_message(std::function<void(std::string, bool binary)> callback){
        on_frame_callback_ = std::move(callback);
    }

    void websocket_connection::start_read_loop()
    {
        co_spawn(ws_->get_io_context(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                co_await read_loop();
            },
            detached);
    }

    awaitable<void> websocket_connection::read_loop()
    {
        try {
            while (ws_->is_open()) {
                LOG_LEVEL(2, "waiting websocket data");

                auto bytes_transferred = co_await ws_->read_some(buffer_.write_position(), buffer_.write_capacity());

                LOG_LEVEL(2, "socket read: {} bytes", bytes_transferred);

                // commit_write the read data
                buffer_.commit_write(bytes_transferred);

                // get remaining data in the frame
                auto remaining = ws_->remaining_in_frame();

                // no pending data to read from the frame
                if(remaining==0){

                    // is the message complete ? FIN flag is set
                    if(ws_->is_message_complete()){

                        // check if the message is a valid UTF8 message
                        if(!ws_->is_binary()){
                            if(utf8_naive(buffer_.data(), buffer_.size()) > 0){
                                LOG_ERROR("invalid UTF8 message received!");
                                co_return;
                            }
                        }

                        if (on_frame_callback_) {
                            std::string data(reinterpret_cast<const char *>(buffer_.data()), buffer_.size());
                            LOG_DEBUG("decoded payload: '{}'", util::lowercase_hex_encode(data));
                            on_frame_callback_(std::move(data), ws_->is_binary());
                        }

                        // clear processed buffer
                        buffer_.commit_read(buffer_.size());
                    }

                }else{
                    // if the remaining data is larger than the buffer, then reserve more space
                    if(buffer_.write_capacity() < remaining){

                        // check if the buffer is not going to overflow
                        if(buffer_.size() + remaining > MAX_BUFFER_SIZE){
                            LOG_ERROR("websocket buffer overflow. closing connection");
                            co_return;
                        }

                        // reserve more space
                        if(buffer_.reserve_write_capacity(remaining, BUFFER_GROWING_SIZE)){
                            LOG_DEBUG("buffer resized to {} bytes", buffer_.capacity());
                        }else{
                            LOG_ERROR("error resizing buffer to accommodate {} bytes", remaining);
                            co_return;
                        }
                    }
                }
            }
        } catch (const boost::system::system_error& e) {
            if (e.code() != boost::asio::error::operation_aborted &&
                e.code() != boost::asio::error::eof) {
                LOG_ERROR("websocket read error: {}", e.what());
            }
        }
    }

    void websocket_connection::process_out_queue()
    {
        if(out_queue_.empty() || writing_) return;
        writing_ = true;

        co_spawn(ws_->get_io_context(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                try {
                    while(!out_queue_.empty()) {
                        LOG_LEVEL(2, "handling websocket write, remaining in queue: {}", out_queue_.size());
                        auto& data = out_queue_.front();
                        ws_->set_binary(data.second);

                        co_await ws_->write(std::string_view(data.first));

                        LOG_DEBUG("message sent, remaining in queue: {}", out_queue_.size());
                        out_queue_.pop();
                    }
                    writing_ = false;
                } catch (const boost::system::system_error& e) {
                    if(e.code() != boost::asio::error::operation_aborted){
                        LOG_ERROR("error while writing to websocket: {}", e.what());
                    }
                    writing_ = false;
                }
            },
            detached);
    }

    std::shared_ptr<asio::socket> websocket_connection::release_socket(){
        // cancel pending async i/io requests on this socket
        ws_->cancel();

        // return socket
        return ws_;
    }

    void websocket_connection::start(){
        // reserve default buffer size
        if(!buffer_.reserve(DEFAULT_BUFFER_SIZE)){
            LOG_ERROR("cannot allocate default websocket buffer: {}", DEFAULT_BUFFER_SIZE);
            return;
        }

        // handle timeout on websocket
        ws_->start_timeout();

        // initiates async reading on socket
        start_read_loop();
    }

    void websocket_connection::stop(){
        execute([this, self = shared_from_this()]{
            LOG_LEVEL(1, "closing websocket");
            co_spawn(ws_->get_io_context(),
                [this, self]() -> awaitable<void> {
                    try {
                        co_await ws_->close_graceful();
                        LOG_LEVEL(1, "websocket closed gracefully");
                    } catch (const boost::system::system_error& e) {
                        LOG_LEVEL(1, "websocket close result: {}", e.what());
                    }
                },
                detached);
        });
    }

    bool websocket_connection::congested_connection(){
        return out_queue_.size()>=MAX_OUTPUT_MESSAGES;
    }

    void websocket_connection::send_binary(std::string data){
        execute([this, data = std::move(data)]() mutable {
            // stop pushing more messages if the connection is congested
            if(congested_connection()){
                LOG_WARNING("websocket is congested. discarding packets!");
                return;
            }

            LOG_LEVEL(2, "adding frame to websocket queue");
            out_queue_.emplace(std::move(data), true);
            process_out_queue();
        });
    }

    void websocket_connection::send_text(std::string text){
        execute([this, data = std::move(text)]() mutable {
            // stop pushing more messages if the connection is congested
            if(congested_connection()){
                LOG_WARNING("websocket is congested. discarding packets!");
                return;
            }

            LOG_LEVEL(2, "adding frame to websocket queue");
            out_queue_.emplace(std::move(data), false);
            process_out_queue();
        });
    }

}

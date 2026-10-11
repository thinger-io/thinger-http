#ifndef SSE_CONNECTION_HPP
#define SSE_CONNECTION_HPP

#include <memory>
#include <queue>
#include "../common/http_request.hpp"
#include "../common/http_response.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../data/out_string.hpp"
#include "../../util/logger.hpp"
#include "../../util/types.hpp"
#include "connection_tracker.hpp"

namespace thinger::http{

class sse_connection : public std::enable_shared_from_this<sse_connection>, public boost::noncopyable{

public:

    /**
     * Parameter for controlling the maximum number of pending messages stored
     * in the output queue.
     */
    static const int MAX_OUTPUT_MESSAGES = 100;

    sse_connection(std::shared_ptr<asio::socket> socket) :
            socket_(socket),
            timer_(socket->get_io_context()),
            idle_(false)
    {
        LOG_DEBUG("created sse connection");
    }

    virtual ~sse_connection()
    {
        LOG_DEBUG("releasing sse connection");
    }

private:

    void handle_timeout()
    {
        idle_ = true;
        timer_.expires_after(std::chrono::seconds(60));
        timer_.async_wait(
            [this, self = shared_from_this()](const boost::system::error_code& e){
                // if timer was not cancelled (just expired by itself)
                if(e != boost::asio::error::operation_aborted && !idle_){
                    handle_timeout();
                }else if(idle_){
                    // will terminate any pending async reads or writes
                    close();
                }
            }
        );
    }

    void process_out_queue()
    {
        if(writing_) return;
        if(out_queue_.empty()) return;

        writing_ = true;

        // Spawn a coroutine to handle the write
        co_spawn(socket_->get_io_context(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                    while(!out_queue_.empty()) {
                        const auto& data = out_queue_.front();
                        std::vector<boost::asio::const_buffer> buffers;
                        buffers.push_back(boost::asio::buffer(data.first));
                        buffers.push_back(boost::asio::buffer(misc_strings::name_value_separator));
                        buffers.push_back(boost::asio::buffer(data.second));
                        buffers.push_back(boost::asio::buffer(misc_strings::lf));

                        if(data.first == "data"){
                            buffers.push_back(boost::asio::buffer(misc_strings::lf));
                        }

                        auto [write_ec, write_bytes] = co_await socket_->write(buffers);
                        if (write_ec) {
                            // nothing else can be sent
                            close();
                            break;
                        }
                        idle_ = false;
                        out_queue_.pop();
                    }
                    writing_ = false;
            },
            detached);
    }

    // Close the connection (on its thread)
    void close();

    // Taken over from a server: told when it shuts down
    void track_shutdown();

public:

    void start(){
        track_shutdown();
        handle_timeout();
    }

    void stop(){
        boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this()](){
            close();
        });
    }

    /**
     * Set what to do when the server shuts down gracefully (see http_server_base::shutdown()),
     * on the connection thread: e.g. send a last event and stop(). By default the connection
     * is closed (clients reconnect). Whatever is open at the end of the grace period is closed
     * anyway
     */
    void on_shutdown(std::function<void()> callback){
        on_shutdown_callback_ = std::move(callback);
    }

    void send_retry(unsigned long millis){
        handle_write("retry", boost::lexical_cast<std::string>(millis));
    }

    void send_event(const std::string& event_name){
        handle_write("event", event_name);
    }

    void send_data(const std::string& data){
        handle_write("data", data);
    }

    void handle_write(const std::string& type, const std::string& value){
         boost::asio::dispatch(socket_->get_io_context(), [this, self = shared_from_this(), type, value](){
            if(out_queue_.size()<=MAX_OUTPUT_MESSAGES){
                out_queue_.push(std::make_pair(type, value));
                process_out_queue();
            }
        });
    }

private:
    /// Socket being used HTTP connection
    std::shared_ptr<asio::socket> socket_;

    /// Out queue
    std::queue<std::pair<std::string, std::string>> out_queue_;

    bool writing_ = false;

    /// Timer used for controlling HTTP timeout
    boost::asio::steady_timer timer_;

    bool idle_;

    /// Server shutdown callback
    std::function<void()> on_shutdown_callback_;

    /// The server tracking this connection, if any (see connection_tracker.hpp)
    std::shared_ptr<taken_over_connection> tracking_;
};

}

#endif
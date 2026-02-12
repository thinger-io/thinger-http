#ifndef THINGER_WEBSOCKET_CONNECTION_HPP
#define THINGER_WEBSOCKET_CONNECTION_HPP

#include <memory>
#include <queue>
#include "../util/data_buffer.hpp"
#include "../../asio/sockets/websocket.hpp"
#include "../../asio/sockets/socket.hpp"
#include "../data/out_data.hpp"
#include "../../util/types.hpp"

namespace thinger::http{

class websocket_connection : public std::enable_shared_from_this<websocket_connection>{

public:

    /**
    * Parameter for controlling the maximum number of bytes used in the incoming
    * buffer
    */
    static constexpr int DEFAULT_BUFFER_SIZE = 1024;        // 1KB
    static constexpr int BUFFER_GROWING_SIZE = 1024;        // 1KB
    static constexpr int MAX_BUFFER_SIZE = 16*1024*1024;    // 16MB

    /**
    * Parameter for controlling the maximum number of pending messages stored
    * in the output queue.
    */
    static constexpr int MAX_OUTPUT_MESSAGES = 100;

    /**
     * Parameter for controlling the number of live http client connections
     */
    static std::atomic<unsigned long> connections;

    /**
     * Constructor that requires a socket
     * @param socket
     */
    websocket_connection(std::shared_ptr<asio::websocket> socket);

    /**
     * Destructor
     */
    virtual ~websocket_connection();

    /**
     * Set a callbback to listen for text frames received
     * @param callback
     */
    void on_message(std::function<void(std::string, bool binary)> callback);

    /**
     * Set a message handler
     * @param handler
     */
    //void set_stream_handler(std::shared_ptr<messages::stream_handler> handler);


    /**
     * Execute actions on the websocket thread
     * @param callback
     */
    inline void execute(std::function<void()> callback){
        boost::asio::dispatch(ws_->get_io_context(), [self = shared_from_this(), callback = std::move(callback)](){
            callback();
        });
    }

    /**
     * Start the websocket. Required for control lifetime and reading messages
     */
    void start();

    /**
     * Stop the webbsocket
     */
    void stop();

    /**
     *
     * @return true if the endpoint connection is congested
     */
    bool congested_connection();

    /**
     * Send a text frame over connection
     * @param text
     */
    void send_text(std::string text);

    /**
       * Send a binary frame over connection
       * @param text
       */
    void send_binary(std::string data);

    /**
    * Return the base socket used in this client connection and release it for being used by another connection handler.
    * No further calls mut be done to this instance.
    */
    std::shared_ptr<asio::socket> release_socket();

private:

    /**
     * Start the read loop coroutine
     */
    void start_read_loop();

    /**
     * Main read loop coroutine
     */
    awaitable<void> read_loop();

    /**
     * Process the output queue
     */
    void process_out_queue();

private:

    /// Socket being used for the websocket connection
    std::shared_ptr<asio::websocket> ws_;

    /// Shared keeper to keep the connection alive
    //std::shared_ptr<base::shared_keeper<websocket_connection>> shared_keeper_;

    /// Out queue
    std::queue<std::pair<std::string, bool>> out_queue_;

    /// Buffer for incoming data.
    thinger::data_buffer buffer_;

    /// Message listener
    //std::shared_ptr<messages::stream_handler> stream_handler_;

    /// Text frame callback
    std::function<void(std::string, bool)> on_frame_callback_;

    bool writing_ = false;
};

}

#endif
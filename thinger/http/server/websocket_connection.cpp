#include "../../util/logger.hpp"
#include "../../util/hex.hpp"
#include "../../util/types.hpp"
#include "websocket_connection.hpp"
#include <utility>
#include "../util/utf8.hpp"

namespace thinger::http{

    // A sender waiting for its message to be written (send_*_async())
    struct websocket_connection::send_waiter {
        explicit send_waiter(boost::asio::io_context& io_context) :
            timer(io_context, boost::asio::steady_timer::time_point::max()) {}

        void finish(bool success){
            done = true;
            written = success;
            timer.cancel();
        }

        awaitable<bool> wait(){
            if(!done) co_await timer.async_wait(use_nothrow_awaitable);
            co_return written;
        }

        boost::asio::steady_timer timer;
        bool done = false;
        bool written = false;
    };

    websocket_connection::websocket_connection(std::shared_ptr<asio::websocket> socket) :
        ws_(std::move(socket)),
        flow_(ws_->get_io_context(), write_limits{.max_messages = MAX_OUTPUT_MESSAGES}),
        resume_timer_(ws_->get_io_context(), boost::asio::steady_timer::time_point::max())
    {
        LOG_LEVEL(2, "websocket connection created");
    }

    websocket_connection::~websocket_connection()
    {
        LOG_LEVEL(1, "releasing websocket connection");
    }

    void websocket_connection::on_shutdown(sync_callback<void()> callback){
        on_shutdown_callback_ = std::move(callback);
    }

    void websocket_connection::server_shutdown(){
        if(on_shutdown_callback_){
            on_shutdown_callback_();
            return;
        }
        LOG_LEVEL(1, "closing websocket: the server shuts down");
        co_spawn(ws_->get_io_context(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                co_await close_connection(asio::websocket::CLOSE_GOING_AWAY);
            },
            detached);
    }

    awaitable<void> websocket_connection::close_connection(uint16_t code){
        co_await ws_->close_graceful(code);
        wake_reader();
    }

    void websocket_connection::on_message(sync_callback<void(std::string, bool binary)> callback){
        on_frame_callback_ = std::move(callback);
    }

    void websocket_connection::start_read_loop()
    {
        co_spawn(ws_->get_io_context(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                // RAII guard: clears the on_message callback when the coroutine
                // frame is destroyed (normal exit, exception, or io_context
                // teardown). This breaks shared_ptr cycles that occur when the
                // user captures shared_ptr<websocket_connection> in the callback.
                struct cycle_guard {
                    websocket_connection& connection;
                    ~cycle_guard() {
                        connection.on_frame_callback_ = nullptr;
                        connection.on_shutdown_callback_ = nullptr;
                        // the watermark callbacks too, and producers waiting are told
                        connection.flow_.close();
                        // closed: no longer counts for the server
                        if (connection.tracking_) connection.tracking_->closed();
                    }
                } guard{*this};

                co_await read_loop();
            },
            detached);
    }

    awaitable<void> websocket_connection::read_loop()
    {
        size_t next_read_size = DEFAULT_BUFFER_SIZE;

        while (ws_->is_open()) {
            // nothing is read while paused
            if (paused_ && !co_await wait_resumed()) break;

            LOG_LEVEL(2, "waiting websocket data");

            auto buf = buffer_.prepare(next_read_size);
            auto [ec, bytes_transferred] = co_await ws_->read_message_some(
                static_cast<uint8_t*>(buf.data()), buf.size());

            if (ec) {
                break;
            }

            LOG_LEVEL(2, "socket read: {} bytes", bytes_transferred);

            buffer_.commit(bytes_transferred);

            // get remaining data in the frame
            auto remaining = ws_->remaining_in_frame();

            // no pending data to read from the frame
            if(remaining == 0){

                // is the message complete ? FIN flag is set
                if(ws_->is_message_complete() && !deliver_message()){
                    ws_->close(asio::websocket::CLOSE_INVALID_PAYLOAD);
                    co_return;
                }

                next_read_size = DEFAULT_BUFFER_SIZE;

            }else{
                // check if the buffer is not going to overflow
                if(buffer_.size() + remaining > buffer_.max_size()){
                    LOG_ERROR("websocket buffer overflow. closing connection");
                    ws_->close(asio::websocket::CLOSE_MESSAGE_TOO_BIG);
                    co_return;
                }

                // next iteration will prepare enough space for remaining frame data
                next_read_size = static_cast<size_t>(remaining);
            }
        }
    }

    bool websocket_connection::deliver_message()
    {
        auto readable = buffer_.data();
        auto* data_ptr = static_cast<const uint8_t*>(readable.data());
        auto data_size = readable.size();

        // check if the message is a valid UTF8 message
        if(!ws_->is_binary() && !utf8::is_valid(data_ptr, data_size)){
            LOG_ERROR("invalid UTF8 message received!");
            return false;
        }

        if (on_frame_callback_) {
            std::string data(reinterpret_cast<const char*>(data_ptr), data_size);
            LOG_DEBUG("decoded payload: '{}'", util::lowercase_hex_encode(data));
            on_frame_callback_(std::move(data), ws_->is_binary());
        }

        // clear processed buffer
        buffer_.consume(buffer_.size());
        return true;
    }

    awaitable<bool> websocket_connection::wait_resumed()
    {
        LOG_LEVEL(2, "websocket reading paused");
        while (paused_ && !released_ && ws_->is_open()) {
            co_await resume_timer_.async_wait(use_nothrow_awaitable);
        }
        co_return !released_ && ws_->is_open();
    }

    void websocket_connection::wake_reader(){
        resume_timer_.cancel();
    }

    void websocket_connection::suspend_keepalive(){
        keepalive_ping_ = ws_->get_ping_interval();
        keepalive_idle_ = ws_->get_idle_timeout();
        ws_->set_keepalive(std::chrono::milliseconds{0}, std::chrono::milliseconds{0});
    }

    void websocket_connection::restore_keepalive(){
        ws_->set_keepalive(keepalive_ping_, keepalive_idle_);
    }

    void websocket_connection::pause_reading(){
        execute([this]{
            if(paused_) return;
            paused_ = true;
            if(started_ && !released_) suspend_keepalive();
        });
    }

    void websocket_connection::resume_reading(){
        execute([this]{
            if(!paused_) return;
            paused_ = false;
            if(started_ && !released_) restore_keepalive();
            wake_reader();
        });
    }

    bool websocket_connection::is_reading_paused() const{
        return paused_;
    }

    void websocket_connection::process_out_queue()
    {
        if(out_queue_.empty() || writing_) return;
        writing_ = true;
        co_spawn(ws_->get_io_context(), write_loop(shared_from_this()), detached);
    }

    awaitable<void> websocket_connection::write_loop([[maybe_unused]] std::shared_ptr<websocket_connection> self)
    {
        while(!out_queue_.empty() && ws_->is_open()) {
            LOG_LEVEL(2, "handling websocket write, remaining in queue: {}", out_queue_.size());
            auto message = std::move(out_queue_.front());
            out_queue_.pop_front();
            ws_->set_binary(message.binary);

            watch_write();
            auto [write_ec, write_bytes] = co_await ws_->write(std::string_view(message.data));
            flow_.write_finished();

            flow_.remove(message.data.size());
            if(message.waiter) message.waiter->finish(!write_ec);

            if (write_ec) {
                // a websocket that cannot write is broken: closed, which ends reading too
                LOG_LEVEL(1, "websocket write failed: {}", write_ec.message());
                if(write_ec != boost::asio::error::operation_aborted) ws_->close();
                break;
            }

            LOG_DEBUG("message sent, remaining in queue: {}", out_queue_.size());
        }

        if(!ws_->is_open() || !out_queue_.empty()){
            discard_queue();
            wake_reader();
        }
        writing_ = false;
    }

    void websocket_connection::discard_queue()
    {
        // closed first: nobody is told the queue drained
        flow_.close();
        auto discarded = std::move(out_queue_);
        out_queue_.clear();
        flow_.clear();
        for(auto& message : discarded){
            if(message.waiter) message.waiter->finish(false);
        }
    }

    void websocket_connection::watch_write()
    {
        if(flow_.limits().write_timeout.count() <= 0) return;
        flow_.write_started([weak = weak_from_this()]{
            if(auto self = weak.lock()) self->close_congested("its write timed out");
        });
    }

    void websocket_connection::close_congested(const char* reason)
    {
        if(!ws_->is_open()) return;
        LOG_WARNING("closing websocket: {}", reason);
        ws_->close(asio::websocket::CLOSE_TRY_AGAIN_LATER);
        wake_reader();
    }

    std::shared_ptr<asio::socket> websocket_connection::release_socket(){
        // the read loop ends, even if paused
        released_ = true;
        wake_reader();

        // the new owner decides about the keepalive started by start()
        ws_->set_keepalive(std::chrono::milliseconds{0}, std::chrono::milliseconds{0});

        // cancel pending async i/io requests on this socket (a frame being written is
        // completed by the next write)
        ws_->cancel();

        // return socket
        return ws_;
    }

    void websocket_connection::start(){
        started_ = true;

        // Taken over from a server: told when it shuts down
        tracking_ = taken_over_connection::of(*ws_);
        if(tracking_){
            tracking_->set_type(connection_type::websocket);
            tracking_->on_shutdown([weak = weak_from_this()]{
                if(auto self = weak.lock()) self->server_shutdown();
            });
        }

        // handle timeout on websocket (suspended while paused)
        ws_->start_timeout();
        if(paused_) suspend_keepalive();

        // initiates async reading on socket
        start_read_loop();
    }

    void websocket_connection::stop(){
        execute([this, self = shared_from_this()]{
            LOG_LEVEL(1, "closing websocket");
            co_spawn(ws_->get_io_context(),
                [this, self]() -> awaitable<void> {
                    co_await close_connection(asio::websocket::CLOSE_NORMAL);
                    LOG_LEVEL(1, "websocket closed");
                },
                detached);
        });
    }

    bool websocket_connection::queue_full() const{
        return flow_.overflows(1);
    }

    void websocket_connection::send_binary(std::string data){
        execute([this, data = std::move(data)]() mutable {
            push_message(std::move(data), true);
        });
    }

    void websocket_connection::send_text(std::string text){
        execute([this, data = std::move(text)]() mutable {
            push_message(std::move(data), false);
        });
    }

    void websocket_connection::push_message(std::string data, bool binary){
        // a full queue: the overflow policy decides
        if(flow_.overflows(data.size()) && !make_room(data.size())) return;

        LOG_LEVEL(2, "adding frame to websocket queue");
        enqueue(out_message{std::move(data), binary, nullptr});
    }

    bool websocket_connection::make_room(size_t size){
        switch(flow_.limits().policy){
            case overflow_policy::drop_oldest:
                // not enough to drop: the newest message goes instead
                if(drop_oldest(size)) return true;
                break;
            case overflow_policy::close:
                close_congested("it is congested");
                return false;
            case overflow_policy::drop_newest:
                break;
        }
        LOG_WARNING("websocket is congested. discarding packets!");
        return false;
    }

    bool websocket_connection::drop_oldest(size_t size){
        size_t bytes = 0;
        size_t messages = 0;
        auto it = out_queue_.begin();
        while(it != out_queue_.end() && flow_.overflows(size, bytes, messages)){
            // a sender waits for it: never dropped
            if(it->waiter){
                ++it;
                continue;
            }
            LOG_LEVEL(2, "websocket is congested. discarding the oldest message");
            bytes += it->data.size();
            ++messages;
            it = out_queue_.erase(it);
        }
        // accounted for once the queue is consistent
        flow_.remove(bytes, messages);
        return !flow_.overflows(size);
    }

    void websocket_connection::enqueue(out_message message){
        flow_.add(message.data.size());
        out_queue_.push_back(std::move(message));
        process_out_queue();
    }

    awaitable<bool> websocket_connection::send_async(std::string data, bool binary){
        auto waiter = std::make_shared<send_waiter>(ws_->get_io_context());
        enqueue(out_message{std::move(data), binary, waiter});
        co_return co_await waiter->wait();
    }

    awaitable<bool> websocket_connection::send_text_async(std::string text){
        return on_io_context(ws_->get_io_context(), shared_from_this(), send_async(std::move(text), false));
    }

    awaitable<bool> websocket_connection::send_binary_async(std::string data){
        return on_io_context(ws_->get_io_context(), shared_from_this(), send_async(std::move(data), true));
    }

    awaitable<bool> websocket_connection::wait_writable(){
        return on_io_context(ws_->get_io_context(), shared_from_this(), flow_.wait_writable());
    }

    void websocket_connection::set_write_limits(const write_limits& limits){
        execute([this, limits]{ flow_.set_limits(limits); });
    }

    write_limits websocket_connection::get_write_limits() const{
        return flow_.limits();
    }

    size_t websocket_connection::queued_bytes() const{
        return flow_.queued_bytes();
    }

    size_t websocket_connection::queued_messages() const{
        return flow_.queued_messages();
    }

    void websocket_connection::on_high_watermark(sync_callback<void()> callback){
        execute([this, callback = std::move(callback)]() mutable { flow_.on_high_watermark(std::move(callback)); });
    }

    void websocket_connection::on_low_watermark(sync_callback<void()> callback){
        execute([this, callback = std::move(callback)]() mutable { flow_.on_low_watermark(std::move(callback)); });
    }

}

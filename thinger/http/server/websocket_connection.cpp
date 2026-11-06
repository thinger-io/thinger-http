#include "../../util/logger.hpp"
#include "../../util/types.hpp"
#include "websocket_connection.hpp"
#include "connection_tracker.hpp"
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include "../util/utf8.hpp"
#include "detail/push_queue.hpp"

namespace thinger::http{

    namespace {

        // The read buffer a connection keeps between messages: a larger message gives it up
        constexpr size_t RETAINED_BUFFER_SIZE = 64 * 1024;

        // A streambuf has no shrink (consume() keeps its capacity) and cannot be moved, so a new
        // one takes its place, with the same largest message (noexcept: it is never left destroyed)
        void release_capacity(boost::asio::streambuf& buffer) noexcept {
            auto max_size = buffer.max_size();
            std::destroy_at(&buffer);
            std::construct_at(&buffer, max_size);
        }

    }

    websocket_connection::websocket_connection(std::shared_ptr<asio::websocket> socket,
                                               const websocket_accept_options& options, server_scope scope) :
        ws_(std::move(socket)),
        queue_(std::make_unique<detail::push_queue<out_message>>(ws_->get_executor(),
            write_limits{.max_messages = MAX_OUTPUT_MESSAGES},
            detail::push_queue<out_message>::hooks{
                .write_item = [this](out_message& message){ return write_message(message); },
                .item_size = [](const out_message& message){ return message.data.size(); },
                // every message is a unit: drop_oldest drops messages
                .ends_unit = [](const out_message&){ return true; },
                .on_failure = [this](detail::push_failure failure, boost::system::error_code ec){ queue_failed(failure, ec); },
                .owner = [this]{ return std::shared_ptr<void>(shared_from_this()); },
                .send_backlog = [this]{ return ws_->send_backlog(); }})),
        buffer_(std::max<size_t>(options.max_message_size, 1)),
        on_close_(ws_->get_executor()),
        scope_(std::move(scope)),
        resumed_(ws_->get_executor()),
        ping_interval_(options.ping_interval.value_or(asio::websocket::CONNECTION_TIMEOUT_SECONDS)),
        idle_timeout_(options.idle_timeout.value_or(2 * asio::websocket::CONNECTION_TIMEOUT_SECONDS))
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
        start_close(asio::websocket::CLOSE_GOING_AWAY);
    }

    void websocket_connection::start_close(uint16_t code, std::string reason){
        // One close frame: the first close decides
        if(closing_) return;
        closing_ = true;
        // The connection is told when the handshake ends (closed, or failed), as it may be paused
        co_spawn(ws_->get_executor(), ws_->close_graceful(code, std::move(reason)),
            [self = shared_from_this()](const std::exception_ptr&){
                LOG_LEVEL(1, "websocket closed");
                self->wake_reader();
            });
    }

    void websocket_connection::on_message(sync_callback<void(std::string, bool binary)> callback){
        on_frame_callback_ = std::move(callback);
    }

    void websocket_connection::on_close(sync_callback<void(const websocket_close&)> callback){
        on_close_.set(std::move(callback));
    }

    void websocket_connection::start_read_loop()
    {
        co_spawn(ws_->get_executor(),
            [this, self = shared_from_this()]() -> awaitable<void> {
                // However the loop ends (also with the io_context destroyed), the queue closes
                // (its watermark callbacks released, producers waiting told) and the callbacks
                // are released: they may hold this connection
                auto guard = queue_->guard_cycles([this]{
                    on_frame_callback_ = nullptr;
                    on_shutdown_callback_ = nullptr;
                    // never told if destroyed open (e.g. with the io_context)
                    on_close_.release();
                    // closed: no longer counts for the server
                    scope_.closed();
                });
                co_await read_messages();
                // Ended here, on the executor (not by the teardown of the io_context): closed,
                // unless released. Told after the cleanup of the guard, as it is posted
                if (!released_) on_close_.notify(close_status());
            },
            detached);
    }

    awaitable<void> websocket_connection::read_messages()
    {
        try {
            co_await read_loop();
        } catch (const std::exception& e) {
            LOG_ERROR("error handling a websocket message: {}", e.what());
            // (not the socket released by on_message(): it belongs to someone else)
            if (!released_) ws_->close(asio::websocket::CLOSE_INTERNAL_ERROR);
        }
    }

    websocket_close websocket_connection::close_status() const
    {
        if (lost_) return websocket_close{};
        return websocket_close{ws_->get_close_code(), ws_->get_close_reason(), ws_->closed_by_peer()};
    }

    awaitable<void> websocket_connection::read_loop()
    {
        // (never past the largest message: the buffer refuses it)
        size_t next_read_size = std::min(DEFAULT_BUFFER_SIZE, buffer_.max_size());

        // (released from a callback, e.g. on_message(): the socket belongs to someone else)
        while (ws_->is_open() && !released_) {
            // nothing is read while paused
            if (paused_ && !co_await wait_resumed()) break;

            LOG_LEVEL(2, "waiting websocket data");

            auto buf = buffer_.prepare(next_read_size);
            auto [ec, bytes_transferred] = co_await ws_->read_message_some(
                static_cast<uint8_t*>(buf.data()), buf.size());

            if (ec) {
                // The connection is lost (reset, or ended without a close frame): closed. Not lost
                // for how it closed if the close frame was out: the peer just did not answer it
                if (!released_ && ws_->is_open()) {
                    LOG_LEVEL(1, "websocket connection lost: {}", ec.message());
                    lost_ = ws_->get_close_code() == asio::websocket::CLOSE_ABNORMAL;
                    ws_->close();
                }
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

                // the message goes on in another frame, if not complete: it must fit
                next_read_size = std::min(DEFAULT_BUFFER_SIZE, buffer_.max_size() - buffer_.size());
                if(next_read_size == 0){
                    LOG_DEBUG("websocket buffer overflow. closing connection");
                    ws_->close(asio::websocket::CLOSE_MESSAGE_TOO_BIG);
                    co_return;
                }

            }else{
                // check if the buffer is not going to overflow
                if(buffer_.size() + remaining > buffer_.max_size()){
                    LOG_DEBUG("websocket buffer overflow. closing connection");
                    ws_->close(asio::websocket::CLOSE_MESSAGE_TOO_BIG);
                    co_return;
                }

                // The rest of the frame, but the buffer grows only with what arrives (at most
                // doubling), never to the size a header announces: what the buffer already has
                // room for is used as is
                auto received = buffer_.size();
                auto room = buffer_.capacity() > received ? buffer_.capacity() - received : 0;
                next_read_size = static_cast<size_t>(std::min<uint64_t>(remaining,
                    std::max({DEFAULT_BUFFER_SIZE, received, room})));
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
            LOG_DEBUG("invalid UTF8 message received!");
            return false;
        }

        if (on_frame_callback_) {
            std::string data(reinterpret_cast<const char*>(data_ptr), data_size);
            // Only the size: messages may hold credentials (see body_logging)
            LOG_TRACE("websocket {} message received: {} bytes", ws_->is_binary() ? "binary" : "text", data_size);
            on_frame_callback_(std::move(data), ws_->is_binary());
        }

        // clear processed buffer, giving up what only a large message needed
        if (data_size > RETAINED_BUFFER_SIZE) release_capacity(buffer_);
        else buffer_.consume(buffer_.size());
        return true;
    }

    awaitable<bool> websocket_connection::wait_resumed()
    {
        LOG_LEVEL(2, "websocket reading paused");
        while (paused_ && !released_ && ws_->is_open()) {
            resumed_.reset();   // woken by the next wake_reader(), not by one before
            co_await resumed_.wait();
        }
        co_return !released_ && ws_->is_open();
    }

    void websocket_connection::wake_reader(){
        resumed_.set();
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
            // (once closing, the handshake is what reads the socket, until it closes)
            if(!paused_ || closing_) return;
            paused_ = false;
            if(started_ && !released_) restore_keepalive();
            wake_reader();
        });
    }

    bool websocket_connection::is_reading_paused() const{
        return paused_;
    }

    awaitable<boost::system::error_code> websocket_connection::write_message(out_message& message)
    {
        // (closed meanwhile, e.g. by the peer: what is queued is not written)
        if(!ws_->is_open()) co_return boost::asio::error::not_connected;
        LOG_LEVEL(2, "handling websocket write, remaining in queue: {}", queue_->queued_messages());
        ws_->set_binary(message.binary);
        // past a slice, with a write deadline, in slices: each one is progress, so a slow client
        // reading a large message is told from one that stopped reading
        auto [ec, bytes] = message.data.size() > asio::socket::WRITE_SLICE_SIZE && queue_->get_write_limits().write_timeout.count() > 0
            ? co_await ws_->write_message(message.data, [this]{ queue_->write_progressed(); })
            : co_await ws_->write_message(message.data);
        co_return ec;
    }

    void websocket_connection::queue_failed(detail::push_failure failure, boost::system::error_code ec)
    {
        switch(failure){
            case detail::push_failure::overflow:
                close_congested("it is congested");
                return;
            case detail::push_failure::write_timeout:
                close_congested("its write timed out");
                return;
            case detail::push_failure::write_error:
                // closing: nothing more goes after the close frame, and the handshake goes on
                if(closing_) return;
                // a websocket that cannot write is broken: closed, which ends reading too (not
                // the socket released: its write was cancelled)
                LOG_LEVEL(1, "websocket write failed: {}", ec.message());
                if(ec != boost::asio::error::operation_aborted) ws_->close();
                wake_reader();
                return;
        }
    }

    void websocket_connection::close_congested(const char* reason)
    {
        // (once closing, the closing handshake has its own deadline, and the first code decides)
        if(closing_ || !ws_->is_open()) return;
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
        scope_.set_type(connection_type::websocket);
        scope_.on_shutdown([weak = weak_from_this()]{
            if(auto self = weak.lock()) self->server_shutdown();
        });

        // handle timeout on websocket (suspended while paused)
        ws_->set_keepalive(ping_interval_, idle_timeout_);
        if(paused_) suspend_keepalive();

        // initiates async reading on socket
        start_read_loop();
    }

    void websocket_connection::stop(){
        stop(asio::websocket::CLOSE_NORMAL);
    }

    void websocket_connection::stop(uint16_t code, std::string reason){
        if(!asio::websocket::valid_close_code(code)){
            throw std::invalid_argument("websocket close code may not be sent: " + std::to_string(code));
        }
        if(reason.size() > asio::websocket::MAX_CLOSE_REASON_SIZE){
            throw std::invalid_argument("websocket close reason above 123 bytes: " + std::to_string(reason.size()));
        }
        if(!utf8::is_valid(reason)){
            throw std::invalid_argument("websocket close reason is not valid UTF-8");
        }
        execute([this, self = shared_from_this(), code, reason = std::move(reason)]() mutable {
            LOG_LEVEL(1, "closing websocket: {}", code);
            start_close(code, std::move(reason));
        });
    }

    bool websocket_connection::queue_full() const{
        return queue_->full();
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
        // (nothing goes after the close frame, nor once closed, even if the read loop did not end
        // yet and the queue is still open: not a congestion)
        if(closing_ || !ws_->is_open()) return;
        // (a queue closed drops it too: not a congestion either)
        if(queue_->push(out_message{std::move(data), binary}) == detail::push_queue<out_message>::admission::drop && !queue_->closed()){
            LOG_DEBUG("websocket is congested. discarding packets!");
        }
    }

    awaitable<bool> websocket_connection::send_async(std::string data, bool binary){
        return queue_->push_and_wait(out_message{std::move(data), binary});
    }

    awaitable<bool> websocket_connection::send_text_async(std::string text){
        return send_async(std::move(text), false);
    }

    awaitable<bool> websocket_connection::send_binary_async(std::string data){
        return send_async(std::move(data), true);
    }

    awaitable<bool> websocket_connection::wait_writable(){
        return queue_->wait_writable();
    }

    void websocket_connection::set_write_limits(const write_limits& limits){
        queue_->set_write_limits(limits);
    }

    write_limits websocket_connection::get_write_limits() const{
        return queue_->get_write_limits();
    }

    size_t websocket_connection::queued_bytes() const{
        return queue_->queued_bytes();
    }

    size_t websocket_connection::queued_messages() const{
        return queue_->queued_messages();
    }

    uint64_t websocket_connection::dropped_messages() const{
        return queue_->dropped_messages();
    }

    void websocket_connection::on_high_watermark(sync_callback<void()> callback){
        queue_->on_high_watermark(std::move(callback));
    }

    void websocket_connection::on_low_watermark(sync_callback<void()> callback){
        queue_->on_low_watermark(std::move(callback));
    }

}

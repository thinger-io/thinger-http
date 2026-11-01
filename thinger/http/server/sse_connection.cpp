#include "sse_connection.hpp"
#include "connection_tracker.hpp"
#include <cassert>
#include <string_view>
#include <vector>
#include "../../asio/io_context_of.hpp"
#include "../../util/logger.hpp"
#include "detail/push_queue.hpp"

namespace thinger::http {

    namespace {
        constexpr std::string_view keepalive_comment = ": keep-alive\n";

        /// Whether `value` holds a character that would end a field
        bool breaks_field(std::string_view value, bool nul = false){
            return value.find_first_of(nul ? std::string_view("\r\n\0", 3) : std::string_view("\r\n")) != std::string_view::npos;
        }

        /// Append the field `name: value`: false, nothing appended, if a line break in the value
        /// (or a NUL in an id) would end it and inject fields or events
        bool append_field(std::string& out, std::string_view name, std::string_view value){
            // the names are the fields of the protocol: no line breaks, nor the colon of the value
            assert(!name.empty() && !breaks_field(name) && name.find(':') == std::string_view::npos);
            bool id = name == "id";
            if(breaks_field(value, id)){
                LOG_ERROR("sse field refused: the value of '{}' holds a line break{}", name, id ? " or NUL" : "");
                return false;
            }
            out.append(name).append(misc_strings::name_value_separator).append(value).append(misc_strings::lf);
            return true;
        }

        /// Append the data of an event, which ends it: one data field per line (CRLF, CR or LF
        /// end a line, WHATWG HTML 9.2.6), then the empty line dispatching it
        void append_data(std::string& out, std::string_view value){
            out.reserve(out.size() + value.size() + 8);
            for(;;){
                auto end = value.find_first_of("\r\n");
                out.append("data").append(misc_strings::name_value_separator)
                   .append(value.substr(0, end)).append(misc_strings::lf);
                if(end == std::string_view::npos) break;
                auto next = end + 1;
                if(value[end] == '\r' && next < value.size() && value[next] == '\n') ++next;
                value.remove_prefix(next);
            }
            out.append(misc_strings::lf);
        }

        /// An event as written, or nothing if one of its fields is refused
        std::optional<std::string> format_event(const sse_event& event){
            std::string out;
            if(event.event && !append_field(out, "event", *event.event)) return std::nullopt;
            if(event.id && !append_field(out, "id", *event.id)) return std::nullopt;
            if(event.retry){
                if(event.retry->count() < 0){
                    LOG_ERROR("sse field refused: a negative retry");
                    return std::nullopt;
                }
                append_field(out, "retry", std::to_string(event.retry->count()));
            }
            append_data(out, event.data);
            return out;
        }

        awaitable<bool> refused(){
            co_return false;
        }
    }

    // (sse_options() and not sse_options{}: the structure budget reads braces at namespace
    // scope as a body)
    static_assert(sse_options().keepalive == sse_connection::DEFAULT_KEEPALIVE_INTERVAL);

    sse_connection::sse_connection(std::shared_ptr<asio::socket> socket, boost::asio::any_io_executor executor,
                                   server_scope scope) :
            socket_(std::move(socket)),
            executor_(executor ? std::move(executor) : boost::asio::any_io_executor(socket_->get_io_context().get_executor())),
            queue_(std::make_unique<detail::push_queue<out_field>>(executor_,
                write_limits{.max_messages = MAX_OUTPUT_MESSAGES + 1},
                detail::push_queue<out_field>::hooks{
                    .write_item = [this](out_field& field){ return write_field(field); },
                    .item_size = [](const out_field& field){ return field.data.size(); },
                    // an event, or its data field: drop_oldest drops whole events
                    .ends_unit = [](const out_field& field){ return field.ends_event; },
                    .on_failure = [this](detail::push_failure failure, boost::system::error_code ec){ queue_failed(failure, ec); },
                    .owner = [this]{ return std::shared_ptr<void>(shared_from_this()); },
                    .send_backlog = [this]{ return socket_->send_backlog(); }})),
            // a member, called only while this lives: not kept alive while waiting
            keepalive_timer_(executor_, [this]{
                if(auto self = weak_from_this().lock()) self->keepalive_expired();
            }),
            on_close_(executor_),
            scope_(std::move(scope))
    {
        LOG_DEBUG("created sse connection");
    }

    sse_connection::~sse_connection()
    {
        LOG_DEBUG("releasing sse connection");
    }

    void sse_connection::execute(sync_callback<void()> callback){
        if(asio::running_in_this_thread(executor_)){
            callback();
            return;
        }
        boost::asio::dispatch(executor_, [self = shared_from_this(), callback = std::move(callback)](){
            callback();
        });
    }

    void sse_connection::start(){
        execute([this, self = shared_from_this()]{
            track_shutdown();
            arm_keepalive();
            co_spawn(executor_, read_loop(shared_from_this()), detached);
        });
    }

    void sse_connection::stop(){
        execute([this, self = shared_from_this()]{
            close();
        });
    }

    void sse_connection::close(){
        if(closed_) return;
        closed_ = true;
        keepalive_timer_.cancel();
        socket_->close();
        on_shutdown_callback_ = nullptr;
        // nothing else is written: producers waiting are told it closed, nobody that it drained
        queue_->close();
        // no longer counts for the server
        scope_.closed();
        // told last, once, posted (the callback is released here: it may own this connection)
        on_close_.notify();
    }

    awaitable<void> sse_connection::read_loop([[maybe_unused]] std::shared_ptr<sse_connection> self){
        // However the loop ends (also with the io_context destroyed), the queue closes (its
        // watermark callbacks released, producers waiting told) and the callbacks are released:
        // they may hold this connection
        auto guard = queue_->guard_cycles([this]{
            on_shutdown_callback_ = nullptr;
            on_close_.release();
        });

        uint8_t buffer[512];
        while(!closed_){
            auto [ec, bytes] = co_await socket_->read_some(buffer, sizeof(buffer));
            if(ec) break;
        }
        if(!closed_){
            LOG_DEBUG("sse client left");
            close();
        }
    }

    void sse_connection::track_shutdown(){
        scope_.set_type(connection_type::sse);
        // called on the io_context of the socket: the callback and the close run on the executor
        scope_.on_shutdown([weak = weak_from_this()]{
            auto self = weak.lock();
            if(!self) return;
            self->execute([self]{
                if(self->on_shutdown_callback_) self->on_shutdown_callback_();
                else self->close();
            });
        });
    }

    bool sse_connection::send(const sse_event& event){
        auto data = format_event(event);
        if(!data) return false;
        push(out_field{.data = std::move(*data), .ends_event = true});
        return true;
    }

    awaitable<bool> sse_connection::send_async(sse_event event){
        auto data = format_event(event);
        if(!data) return refused();
        return queue_->push_and_wait(out_field{.data = std::move(*data), .ends_event = true});
    }

    void sse_connection::send_retry(unsigned long millis){
        handle_write("retry", std::to_string(millis));
    }

    bool sse_connection::send_event(const std::string& event_name){
        return handle_write("event", event_name);
    }

    bool sse_connection::send_id(const std::string& id){
        return handle_write("id", id);
    }

    void sse_connection::send_data(const std::string& data){
        handle_write("data", data);
    }

    bool sse_connection::send_comment(std::string_view comment){
        if(breaks_field(comment)){
            LOG_ERROR("sse comment refused: it holds a line break");
            return false;
        }
        out_field field;
        field.data.reserve(comment.size() + 3);
        field.data.append(": ").append(comment).append(misc_strings::lf);
        push(std::move(field));
        return true;
    }

    bool sse_connection::handle_write(std::string_view type, std::string_view value){
        out_field field;
        // a data field ends its event
        field.ends_event = type == "data";
        if(field.ends_event) append_data(field.data, value);
        else if(!append_field(field.data, type, value)) return false;
        push(std::move(field));
        return true;
    }

    void sse_connection::push(out_field field){
        execute([this, field = std::move(field)]() mutable {
            // (once closed, nothing is sent: not a congestion)
            if(queue_->push(std::move(field)) == detail::push_queue<out_field>::admission::drop && !queue_->closed()){
                LOG_DEBUG("sse connection is congested. discarding what was sent");
            }
        });
    }

    awaitable<boost::system::error_code> sse_connection::write_field(out_field& field){
        // past a slice, with a write deadline, in slices: each one is progress, so a slow client
        // reading a large event is told from one that stopped reading
        boost::system::error_code ec;
        if(field.data.size() > asio::socket::WRITE_SLICE_SIZE && queue_->get_write_limits().write_timeout.count() > 0){
            std::vector<boost::asio::const_buffer> buffers{boost::asio::buffer(field.data)};
            ec = std::get<0>(co_await socket_->write_slices(buffers, [this]{ queue_->write_progressed(); }));
        }else{
            ec = std::get<0>(co_await socket_->write(std::string_view(field.data)));
        }
        // anything else written puts the keep-alive off
        if(!ec && !field.keepalive) arm_keepalive();
        co_return ec;
    }

    void sse_connection::queue_failed(detail::push_failure failure, boost::system::error_code ec){
        switch(failure){
            case detail::push_failure::overflow:
                LOG_WARNING("sse connection is congested. closing it");
                break;
            case detail::push_failure::write_timeout:
                LOG_WARNING("closing sse connection: its write timed out");
                break;
            case detail::push_failure::write_error:
                // nothing else can be sent
                LOG_DEBUG("sse write failed: {}", ec.message());
                break;
        }
        close();
    }

    void sse_connection::set_keepalive(std::chrono::milliseconds interval){
        execute([this, interval]{
            keepalive_ = interval;
            // the next one counts from now
            keepalive_timer_.cancel();
            arm_keepalive();
        });
    }

    void sse_connection::arm_keepalive(){
        if(closed_ || keepalive_.count() <= 0) return;
        // a later deadline does not touch the timer (see watchdog)
        keepalive_timer_.expires_at(std::chrono::steady_clock::now() + keepalive_);
    }

    void sse_connection::keepalive_expired(){
        if(closed_) return;
        // only when quiet: otherwise what is written moves it later
        queue_->write_idle(out_field{.data = std::string(keepalive_comment), .keepalive = true});
        arm_keepalive();
    }

    awaitable<bool> sse_connection::wait_writable(){
        return queue_->wait_writable();
    }

    void sse_connection::set_write_limits(const write_limits& limits){
        queue_->set_write_limits(limits);
    }

    write_limits sse_connection::get_write_limits() const{
        return queue_->get_write_limits();
    }

    size_t sse_connection::queued_bytes() const{
        return queue_->queued_bytes();
    }

    size_t sse_connection::queued_messages() const{
        return queue_->queued_messages();
    }

    void sse_connection::on_high_watermark(sync_callback<void()> callback){
        queue_->on_high_watermark(std::move(callback));
    }

    void sse_connection::on_low_watermark(sync_callback<void()> callback){
        queue_->on_low_watermark(std::move(callback));
    }

}

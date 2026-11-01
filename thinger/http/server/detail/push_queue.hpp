#ifndef THINGER_HTTP_SERVER_DETAIL_PUSH_QUEUE_HPP
#define THINGER_HTTP_SERVER_DETAIL_PUSH_QUEUE_HPP

#include <deque>
#include <functional>
#include <iterator>
#include <memory>
#include <optional>
#include <utility>
#include <boost/asio/co_spawn.hpp>
#include <boost/asio/detached.hpp>
#include <boost/system/error_code.hpp>
#include "../../../asio/async_event.hpp"
#include "../../../util/types.hpp"
#include "../flow_control.hpp"

namespace thinger::http::detail {

// Why a push_queue stopped writing (see push_queue::hooks::on_failure)
enum class push_failure {
    overflow,       // a message sent without waiting to a full queue, with overflow_policy::close
    write_timeout,  // a write not done within write_limits::write_timeout
    write_error     // write_item() failed: the error it returned
};

// The queue, flow control and single writer that websocket_connection and sse_connection compose
// (target design §4.7). Not public: only those two classes use it, from their .cpp files.
//
// Items are written one at a time, in order, by one writer coroutine, started when the queue
// gets an item and ended once it is empty or closed. A unit is what drop_oldest drops whole: the
// items up to one that ends it (ends_unit()), e.g. a WebSocket message, or the fields of an SSE
// event up to its data. The queue never closes its owner's connection: it stops writing, closes
// (the producers waiting are told false) and tells its owner, through on_failure, which closes.
//
// On its owner's executor, except queued_bytes(), queued_messages() and the calls said to be
// from any thread, which keep the owner alive while they hop to it
template<typename Item>
class push_queue {
public:
    struct hooks {
        // Write one item to the wire: an error ends writing (push_failure::write_error)
        std::function<awaitable<boost::system::error_code>(Item&)> write_item;
        // What the write limits count of an item, in bytes
        size_t (*item_size)(const Item&) = nullptr;
        // Whether an item ends its unit (drop_oldest drops whole units)
        bool (*ends_unit)(const Item&) = nullptr;
        // The queue stopped writing, and closed: the owner closes its connection
        std::function<void(push_failure, boost::system::error_code)> on_failure;
        // The owner, kept alive while the writer runs and while a call from any thread hops to
        // the executor: never held while the queue is idle
        std::function<std::shared_ptr<void>()> owner;
        // The bytes its socket holds that the peer has not taken, if it tells (see
        // write_flow::set_send_backlog())
        sync_callback<size_t()> send_backlog = nullptr;
    };

    // What push() did with an item (see write_flow::admit())
    using admission = write_flow::admission;

    push_queue(boost::asio::any_io_executor executor, write_limits defaults, hooks hooks) :
        executor_(std::move(executor)),
        hooks_(std::move(hooks)),
        flow_(executor_, defaults)
    {
        if(hooks_.send_backlog) flow_.set_send_backlog(std::move(hooks_.send_backlog));
    }

    push_queue(const push_queue&) = delete;
    push_queue& operator=(const push_queue&) = delete;

    // Queue an item sent without waiting, or apply the overflow policy to it: dropped (this
    // one, or older units to make room), or the queue fails with push_failure::overflow
    admission push(Item item){
        if(closed_) return admission::drop;
        auto admitted = flow_.admit(hooks_.item_size(item), [this](size_t size){ return drop_oldest(size); });
        switch(admitted){
            case admission::queue:
                enqueue(entry{std::move(item), nullptr});
                break;
            case admission::drop:
                break;
            case admission::close:
                fail(push_failure::overflow, {});
                break;
        }
        return admitted;
    }

    // From any thread: queue an item and wait until written, true, or false if the queue closed
    // or failed first. Never dropped by the overflow policy, which it may go over; a cancelled
    // wait leaves the item queued
    awaitable<bool> push_and_wait(Item item){
        return on_executor(executor_, hooks_.owner(), wait_written(std::move(item)));
    }

    // Write `item` at once if nothing is queued nor being written: outside the write limits and
    // the units, never dropped (e.g. an SSE keep-alive comment). False if not idle, or closed
    bool write_idle(Item item){
        if(closed_ || writing_ || !queue_.empty()) return false;
        idle_item_ = std::move(item);
        start_writer();
        return true;
    }

    // The item being written made progress (the socket took some of it): its write deadline
    // counts from now. From write_item()
    void write_progressed(){ flow_.write_progressed(); }

    // Whether the next item sent without waiting overflows the queue (not the watermarks)
    bool full() const { return flow_.overflows(1); }

    // From any thread
    void set_write_limits(const write_limits& limits){ flow_.set_limits(hooks_.owner(), limits); }
    const write_limits& get_write_limits() const { return flow_.limits(); }
    void on_high_watermark(sync_callback<void()> callback){ flow_.on_high_watermark(hooks_.owner(), std::move(callback)); }
    void on_low_watermark(sync_callback<void()> callback){ flow_.on_low_watermark(hooks_.owner(), std::move(callback)); }
    awaitable<bool> wait_writable(){ return flow_.wait_writable(hooks_.owner()); }

    // Bytes, and items, not written yet (the one being written included); from any thread
    size_t queued_bytes() const { return flow_.queued_bytes(); }
    size_t queued_messages() const { return flow_.queued_messages(); }

    // Nothing else is written: the watermark callbacks are released, producers waiting (and
    // those of the items queued) are told false, the queue is emptied. Once; an item being
    // written completes, its producer told whether it was written
    void close(){
        if(closed_) return;
        closed_ = true;
        // closed first: nobody is told the queue drained
        flow_.close();
        idle_item_.reset();
        auto discarded = std::move(queue_);
        queue_.clear();
        flow_.clear();
        for(auto& entry : discarded){
            if(entry.waiter) entry.waiter->finish(false);
        }
    }

    bool closed() const { return closed_; }

    // Kept by the owner in its read loop, which lives while the connection is open: however the
    // loop ends, also when its frame is destroyed with the io_context, the queue closes and
    // `release` runs, releasing the owner's callbacks (which may own it: a cycle otherwise)
    class cycle_guard {
    public:
        cycle_guard(push_queue& queue, sync_callback<void()> release) :
            queue_(queue), release_(std::move(release)) {}
        ~cycle_guard(){
            queue_.close();
            if(release_) release_();
        }
        cycle_guard(const cycle_guard&) = delete;
        cycle_guard& operator=(const cycle_guard&) = delete;
    private:
        push_queue& queue_;
        sync_callback<void()> release_;
    };

    [[nodiscard]] cycle_guard guard_cycles(sync_callback<void()> release){
        return cycle_guard{*this, std::move(release)};
    }

private:
    // A producer waiting for its item to be written (push_and_wait())
    struct producer {
        explicit producer(const boost::asio::any_io_executor& executor) : done(executor) {}
        void finish(bool success){
            written = success;
            done.set();
        }
        asio::async_event done;
        bool written = false;
    };

    struct entry {
        Item item;
        std::shared_ptr<producer> waiter;
    };

    using iterator = typename std::deque<entry>::iterator;

    awaitable<bool> wait_written(Item item){
        if(closed_) co_return false;
        auto pending = std::make_shared<producer>(executor_);
        enqueue(entry{std::move(item), pending});
        co_await pending->done.wait();
        co_return pending->written;
    }

    void enqueue(entry queued){
        flow_.add(hooks_.item_size(queued.item));
        queue_.push_back(std::move(queued));
        start_writer();
    }

    void start_writer(){
        if(writing_ || closed_ || (queue_.empty() && !idle_item_)) return;
        writing_ = true;
        co_spawn(executor_, write_loop(hooks_.owner()), boost::asio::detached);
    }

    // Write the items, one at a time (`owner` kept alive meanwhile)
    awaitable<void> write_loop([[maybe_unused]] std::shared_ptr<void> owner){
        while(!closed_ && (idle_item_ || !queue_.empty())){
            entry current;
            // written between units: the unit being written stays as it was
            bool counted = !idle_item_;
            if(idle_item_){
                current.item = std::move(*idle_item_);
                idle_item_.reset();
            }else{
                current = std::move(queue_.front());
                queue_.pop_front();
                unit_open_ = !hooks_.ends_unit(current.item);
            }

            flow_.write_started([this]{ fail(push_failure::write_timeout, {}); });
            auto ec = co_await hooks_.write_item(current.item);
            flow_.write_finished();

            if(counted) flow_.remove(hooks_.item_size(current.item));
            if(current.waiter) current.waiter->finish(!ec);
            if(ec){
                fail(push_failure::write_error, ec);
                break;
            }
        }
        writing_ = false;
    }

    // Stop writing for good, and tell the owner (once)
    void fail(push_failure failure, boost::system::error_code ec){
        if(closed_) return;
        close();
        if(hooks_.on_failure) hooks_.on_failure(failure, ec);
    }

    // Past the unit `it` is in: the item ending it (or the end of the queue)
    iterator unit_end(iterator it){
        while(it != queue_.end() && !hooks_.ends_unit(it->item)) ++it;
        return it == queue_.end() ? it : std::next(it);
    }

    static bool awaited(iterator begin, iterator end){
        for(auto it = begin; it != end; ++it){
            if(it->waiter) return true;
        }
        return false;
    }

    // Drop the oldest whole units, until an item of `size` bytes fits: never the rest of the
    // unit being written, a unit with a producer waiting, nor the last unit while incomplete.
    // False if it does not fit even so
    bool drop_oldest(size_t size){
        auto it = queue_.begin();
        if(unit_open_) it = unit_end(it);
        size_t bytes = 0;
        size_t items = 0;
        while(it != queue_.end() && flow_.overflows(size, bytes, items)){
            auto end = unit_end(it);
            if(!hooks_.ends_unit(std::prev(end)->item)) break;
            if(awaited(it, end)){
                it = end;
                continue;
            }
            for(auto dropped = it; dropped != end; ++dropped) bytes += hooks_.item_size(dropped->item);
            items += static_cast<size_t>(std::distance(it, end));
            it = queue_.erase(it, end);
        }
        // accounted for once the queue is consistent
        flow_.remove(bytes, items);
        return !flow_.overflows(size);
    }

    boost::asio::any_io_executor executor_;
    hooks hooks_;
    // Items not written yet (the one being written is out of it)
    std::deque<entry> queue_;
    // Bytes and items queued against the write limits
    write_flow flow_;
    // An item to write before those queued, outside the limits and the units (write_idle())
    std::optional<Item> idle_item_;
    bool writing_ = false;
    // The last item taken for writing did not end its unit: the items up to its end belong to it
    bool unit_open_ = false;
    bool closed_ = false;
};

}

#endif

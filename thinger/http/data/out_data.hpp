#ifndef THINGER_HTTP_DATA_OUT_DATA_HPP
#define THINGER_HTTP_DATA_OUT_DATA_HPP

#include <memory>
#include <functional>
#include <boost/asio.hpp>
#include "../../asio/sockets/socket.hpp"
#include "../../util/types.hpp"

namespace thinger::http::data {

class out_data {

public:
    out_data() = default;
    virtual ~out_data() = default;

    virtual awaitable<io_result> to_socket(std::shared_ptr<thinger::asio::socket> socket) {
        std::vector<boost::asio::const_buffer> buffer;
        fill_buffer(buffer);
        co_return co_await socket->write(buffer);
    }

    void set_next_data(std::shared_ptr<out_data> data) {
        data_ = data;
    }

    virtual size_t get_size() = 0;
    virtual void to_buffer(std::vector<boost::asio::const_buffer>& buffer) const = 0;

protected:
    void fill_buffer(std::vector<boost::asio::const_buffer>& buffer) {
        to_buffer(buffer);
        if (data_) {
            data_->fill_buffer(buffer);
        }
    }

    virtual bool supports_buffer() {
        return true;
    }

private:
    std::shared_ptr<out_data> data_;
};

}

#endif
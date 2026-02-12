#ifndef HTTP_DATA_FRAME_HPP
#define HTTP_DATA_FRAME_HPP

#include <memory>
#include "http_frame.hpp"

namespace thinger::http{

class http_data : public http_frame{

private:
    std::shared_ptr<data::out_data> data_;
public:

    http_data() = default;

    explicit http_data(std::shared_ptr<data::out_data> data) : data_(std::move(data))
    {}

    ~http_data() override = default;

public:

    void to_buffer(std::vector<boost::asio::const_buffer> &buffer) const override{
        if(data_){
            data_->to_buffer(buffer);
        }
    }

    void set_data(std::shared_ptr<data::out_data> data){
        data_ = std::move(data);
    }

    std::shared_ptr<data::out_data> get_data(){
        return data_;
    }

    size_t get_size() override{
        if(data_){
            return data_->get_size();
        }
        return 0;
    }
};

}

#endif
#ifndef OUT_CHUNK_HPP
#define OUT_CHUNK_HPP

#include <sstream>
#include <iomanip>
#include "out_data.hpp"

namespace thinger::http::data{

class out_chunk : public out_data{

public:
    explicit out_chunk(const std::string& str) : str_(str){
        std::ostringstream ss;
        ss << std::hex << str_.size();
        size_ = ss.str();
    }

    out_chunk() : size_("0"){
    }

    ~out_chunk() override = default;

    size_t get_size() override{
        return str_.size();
    }

    void to_buffer(std::vector<boost::asio::const_buffer>& buffer) const override{
        static const std::string crlf = "\r\n";
        buffer.emplace_back(boost::asio::buffer(size_));
        buffer.emplace_back(boost::asio::buffer(crlf));
        buffer.emplace_back(boost::asio::buffer(str_));
        buffer.emplace_back(boost::asio::buffer(crlf));
    }

private:
    std::string str_;
    std::string size_;
};

}

#endif
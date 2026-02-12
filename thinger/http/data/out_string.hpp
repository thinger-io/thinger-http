#ifndef OUT_STRING_HPP
#define OUT_STRING_HPP

#include "out_data.hpp"

namespace thinger::http::data{

class out_string : public out_data{

public:
    out_string(const std::string& str) : str_(str){
    }

    out_string(){
    }

    virtual ~out_string() {
    }

    std::string& get_string(){
        return str_;
    }

    size_t get_size() override{
        return str_.size();
    }

    void to_buffer(std::vector<boost::asio::const_buffer>& buffer) const override{
        buffer.emplace_back(boost::asio::buffer(str_));
    }

private:
    std::string str_;
};

}

#endif
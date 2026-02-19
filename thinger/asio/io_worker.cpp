#include "io_worker.hpp"
#include "../util/logger.hpp"

namespace thinger::asio{

    io_worker::io_worker() :
        io_{1},
        work_(boost::asio::make_work_guard(io_)){
    }

    io_worker::~io_worker() = default;

    void io_worker::start() {
        io_.run();
    }

    void io_worker::stop(){
        io_.stop();
    }

    boost::asio::io_context& io_worker::get_io_context(){
        return io_;
    }

}
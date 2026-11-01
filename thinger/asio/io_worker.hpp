#ifndef THINGER_ASIO_IO_WORKER_HPP
#define THINGER_ASIO_IO_WORKER_HPP

#include <boost/asio/io_context.hpp>

namespace thinger::asio{

    class io_worker {
    public:
        io_worker();
        virtual ~io_worker();

        void start();
        void stop();
        // Once stopped and no thread runs it: destroy the pending handlers (as destroying the
        // io_context would), keeping the io_context, where sockets and timers may still be closed
        // and destroyed until it is
        void shutdown();
        boost::asio::io_context& get_io_context();

    private:
        struct context : boost::asio::io_context {
            using io_context::io_context;
            using execution_context::shutdown;
        };
        context io_;
        boost::asio::executor_work_guard<boost::asio::io_context::executor_type> work_;
    };

}

#endif
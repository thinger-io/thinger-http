#ifndef HTTP_REQUEST_HANDLER_HPP
#define HTTP_REQUEST_HANDLER_HPP

#include <memory>

namespace thinger::http{

    class request;

    class request_handler{
    public:
        request_handler()= default;
        virtual ~request_handler() = default;
    public:
        virtual bool handle_request(std::shared_ptr<request> request) = 0;
    };

}

#endif
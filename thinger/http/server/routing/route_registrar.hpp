#ifndef THINGER_HTTP_ROUTE_REGISTRAR_HPP
#define THINGER_HTTP_ROUTE_REGISTRAR_HPP

#include <string>
#include "route.hpp"
#include "../../common/http_request.hpp"

namespace thinger::http {

// Route registration methods, one per HTTP method, taking any route callback (see
// route_callable): synchronous or coroutine, with or without the JSON body. They return
// the route, to document it or set its schema. Derived provides
// route& make_route(method, const std::string& path), creating the route to register.
template<typename Derived>
class route_registrar {
public:
    template<route_callable F>
    route& get(const std::string& path, F&& handler) {
        return add(method::GET, path, std::forward<F>(handler));
    }

    template<route_callable F>
    route& post(const std::string& path, F&& handler) {
        return add(method::POST, path, std::forward<F>(handler));
    }

    template<route_callable F>
    route& put(const std::string& path, F&& handler) {
        return add(method::PUT, path, std::forward<F>(handler));
    }

    // delete is a keyword
    template<route_callable F>
    route& del(const std::string& path, F&& handler) {
        return add(method::DELETE, path, std::forward<F>(handler));
    }

    template<route_callable F>
    route& patch(const std::string& path, F&& handler) {
        return add(method::PATCH, path, std::forward<F>(handler));
    }

    template<route_callable F>
    route& head(const std::string& path, F&& handler) {
        return add(method::HEAD, path, std::forward<F>(handler));
    }

    template<route_callable F>
    route& options(const std::string& path, F&& handler) {
        return add(method::OPTIONS, path, std::forward<F>(handler));
    }

private:
    template<typename F>
    route& add(method http_method, const std::string& path, F&& handler) {
        return static_cast<Derived&>(*this).make_route(http_method, path) = std::forward<F>(handler);
    }
};

} // namespace thinger::http

#endif // THINGER_HTTP_ROUTE_REGISTRAR_HPP

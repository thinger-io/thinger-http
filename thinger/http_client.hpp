#ifndef THINGER_HTTP_CLIENT_HPP
#define THINGER_HTTP_CLIENT_HPP

// HTTP Client functionality
#include <thinger/http/client/client.hpp>               // client class (standalone sync)
#include <thinger/http/client/async_client.hpp>          // async_client class (for async with worker threads)
#include <thinger/http/client/websocket_client.hpp>     // websocket_client class (returned by client.websocket())
#include <thinger/http/client/client_response.hpp>
#include <thinger/http/client/form.hpp>                 // form class for form data POST
#include <thinger/http/client/stream_types.hpp>         // stream_info, stream_result for streaming downloads
#include <thinger/http/client/request_builder.hpp>      // request_builder for fluent API

// Common HTTP types needed by client
#include <thinger/http/common/http_request.hpp>
#include <thinger/http/common/http_response.hpp>

// Note: client and async_client are already in thinger::http namespace
// WebSocket can be accessed via:
//   - client.websocket(url) -> returns websocket_client (sync)
//   - async_client.websocket(url, callback) -> callback receives shared_ptr<websocket_client>

#endif // THINGER_HTTP_CLIENT_HPP
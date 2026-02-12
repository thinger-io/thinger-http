#ifndef THINGER_HTTP_SERVER_HPP
#define THINGER_HTTP_SERVER_HPP

// HTTP Server functionality
#include <thinger/http/server/server_standalone.hpp>  // server class (standalone)
#include <thinger/http/server/pool_server.hpp>       // pool_server class (for worker threads)
#include <thinger/http/server/request.hpp>
#include <thinger/http/server/response.hpp>
#include <thinger/http/server/request_handler.hpp>

// Routing
#include <thinger/http/server/routing/route_handler.hpp>
#include <thinger/http/server/routing/route_builder.hpp>

// File server
#include <thinger/http/server/file_server/file_server_handler.hpp>

// WebSocket & SSE support
#include <thinger/http/server/websocket_connection.hpp>
#include <thinger/http/server/sse_connection.hpp>

// Common HTTP types needed by server
#include <thinger/http/common/http_request.hpp>
#include <thinger/http/common/http_response.hpp>

#endif // THINGER_HTTP_SERVER_HPP
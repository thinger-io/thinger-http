#include <thinger/asio/socket_server.hpp>
#include <thinger/asio/workers.hpp>
#include <thinger/http/server/server_connection.hpp>
#include <thinger/http/server/routing/route_handler.hpp>
#include <thinger/http/server/routing/route.hpp>  // For pattern macros
#include <thinger/http/server/response.hpp>
#include <thinger/util/logger.hpp>
#include <nlohmann/json.hpp>

using namespace thinger;


int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with Routing Example");
    
    
    // Start worker threads (uses hardware concurrency by default)
    asio::get_workers().start();
    
    // Create route handler
    auto router = std::make_shared<http::route_handler>();
    
    // Enable CORS
    router->enable_cors(true);
    
    // Define routes using the elegant syntax with multiple signatures
    // 
    // Available signatures:
    // 1. [](http::response& response) - When you only need to send a response
    // 2. [](nlohmann::json& body, http::response& response) - When you only need the JSON body
    // 3. [](http::request& request, http::response& response) - When you need request params/headers
    // 4. [](http::request& request, nlohmann::json& body, http::response& response) - When you need everything
    
    // Response-only signature - when you don't need request data
    (*router)[http::method::GET]["/"] = [](http::response& response) {
        nlohmann::json json = {
            {"message", "Welcome to Thinger HTTP Server with Routing!"},
            {"version", "1.0.0"},
            {"endpoints", {
                "/health",
                "/api/v1/status",
                "/api/v1/users",
                "/api/v1/users/:user",
                "/api/v1/echo"
            }}
        };
        response.json(json);
    };
    
    // Response-only for simple endpoints
    (*router)[http::method::GET]["/health"] = [](http::response& response) {
        response.json({{"status", "ok"}, {"timestamp", std::time(nullptr)}});
    };
    
    (*router)[http::method::GET]["/api/v1/status"] = [](http::response& response) {
        response.json({
            {"server", "thinger-http"},
            {"version", "2.0.0"},
            {"uptime", 12345}
        });
    };
    
    // JSON + response signature - when you only need the body, not the full request
    (*router)[http::method::POST]["/api/v1/echo"] = [](nlohmann::json& body, http::response& response) {
        response.json({
            {"echo", body},
            {"timestamp", std::time(nullptr)}
        });
    };
    
    // Response-only for listing (no request params needed)
    (*router)[http::method::GET]["/api/v1/users"] = [](http::response& response) {
        nlohmann::json users = nlohmann::json::array();
        for (int i = 1; i <= 5; ++i) {
            users.push_back({
                {"id", "user" + std::to_string(i)},
                {"name", "User " + std::to_string(i)},
                {"email", "user" + std::to_string(i) + "@example.com"}
            });
        }
        response.json({{"users", users}});
    };
    
    // User detail with parameter (restricted to alphanumeric, underscore, dash, 1-32 chars)
    (*router)[http::method::GET]["/api/v1/users/:user([a-zA-Z0-9_-]{1,32})"] = [](http::request& request, http::response& response) {
        const auto& user_id = request["user"];
        nlohmann::json json = {
            {"id", user_id},
            {"name", "User " + user_id},
            {"email", user_id + "@example.com"},
            {"created_at", "2024-01-01"}
        };
        response.json(json);
    };
    
    // Create user (POST with JSON body) - doesn't need request params
    (*router)[http::method::POST]["/api/v1/users"] = [](nlohmann::json& body, http::response& response) {
        // Validate required fields
        if (!body.contains("name") || !body.contains("email")) {
            response.error(http::http_response::status::bad_request, "Missing required fields: name, email");
            return;
        }
        
        nlohmann::json json = {
            {"id", "user_" + std::to_string(std::time(nullptr))},
            {"name", body["name"]},
            {"email", body["email"]},
            {"created_at", std::time(nullptr)}
        };
        response.json(json, http::http_response::status::created);
    };
    
    // Devices endpoints
    (*router)[http::method::GET]["/api/v1/users/:user([a-zA-Z0-9_-]{1,32})/devices"] = [](http::request& request, http::response& response) {
        const auto& user_id = request["user"];
        nlohmann::json devices = nlohmann::json::array();
        for (int i = 1; i <= 3; ++i) {
            devices.push_back({
                {"id", "device" + std::to_string(i)},
                {"name", "Device " + std::to_string(i)},
                {"owner", user_id},
                {"status", i % 2 == 0 ? "online" : "offline"}
            });
        }
        response.json({{"devices", devices}});
    };
    
    // Device detail
    (*router)[http::method::GET]["/api/v1/users/:user([a-zA-Z0-9_-]{1,32})/devices/:device([a-zA-Z0-9_-]{1,32})"] = [](http::request& request, http::response& response) {
        const auto& user_id = request["user"];
        const auto& device_id = request["device"];
        
        nlohmann::json json = {
            {"id", device_id},
            {"name", "Device " + device_id},
            {"owner", user_id},
            {"status", "online"},
            {"last_seen", std::time(nullptr)},
            {"properties", {
                {"temperature", 25.5},
                {"humidity", 60},
                {"battery", 85}
            }}
        };
        response.json(json);
    };
    
    // Update device - needs all three: request (for params), body, and response
    (*router)[http::method::PUT]["/api/v1/users/:user([a-zA-Z0-9_-]{1,32})/devices/:device([a-zA-Z0-9_-]{1,32})"] = [](http::request& request, nlohmann::json& body, http::response& response) {
        const auto& user_id = request["user"];
        const auto& device_id = request["device"];
        
        nlohmann::json json = {
            {"id", device_id},
            {"owner", user_id},
            {"updated_at", std::time(nullptr)},
            {"changes", body}
        };
        response.json(json);
    };
    
    // Delete device
    (*router)[http::method::DELETE]["/api/v1/users/:user([a-zA-Z0-9_-]{1,32})/devices/:device([a-zA-Z0-9_-]{1,32})"] = [](http::request& request, http::response& response) {
        const auto& user_id = request["user"];
        const auto& device_id = request["device"];
        
        LOG_INFO("Deleting device %s for user %s", device_id.c_str(), user_id.c_str());
        // Send 204 No Content response
        response.send_response(http::http_response::stock_http_reply(http::http_response::status::no_content));
    };
    
    // Example with numeric ID
    (*router)[http::method::GET]["/api/v1/items/:id([0-9]+)"] = [](http::request& request, http::response& response) {
        const auto& item_id = request["id"];
        nlohmann::json json = {
            {"id", std::stoi(item_id)},
            {"name", "Item #" + item_id},
            {"price", 99.99}
        };
        response.json(json);
    };
    
    // Example with file path (matches everything including slashes)
    (*router)[http::method::GET]["/api/v1/files/:path(.+)"] = [](http::request& request, http::response& response) {
        const auto& file_path = request["path"];
        LOG_INFO("Requested file: %s", file_path.c_str());
        nlohmann::json json = {
            {"file", file_path},
            {"exists", false},
            {"message", "File serving not implemented in this example"}
        };
        response.json(json);
    };
    
    // Example with authentication requirement
    (*router)[http::method::GET]["/api/v1/admin/stats"]
        .auth(http::auth_level::ADMIN)
        .description("Get system statistics (admin only)")
        = [](http::request& request, http::response& response) {
            nlohmann::json stats = {
                {"total_users", 150},
                {"total_devices", 450},
                {"active_connections", 87},
                {"uptime_hours", 720}
            };
            response.json(stats);
        };
    
    // Set fallback handler for static files or 404
    router->set_fallback_handler([](http::request& request, http::response& response) {
        auto http_req = request.get_http_request();
        LOG_INFO("No route found for %s %s", 
                 get_method(http_req->get_method()).c_str(), 
                 http_req->get_uri().c_str());
        response.error(http::http_response::status::not_found, "Route not found");
    });
    
    // Create HTTP server
    std::string port = argc > 1 ? argv[1] : "8090";
    auto http_server = std::make_shared<asio::socket_server>("0.0.0.0", port);
    
    // Set up connection handler
    http_server->set_handler([router](std::shared_ptr<asio::socket> socket) {
        // Create HTTP connection
        auto connection = std::make_shared<http::server_connection>(socket);
        
        // Set request handler
        connection->set_handler([router](std::shared_ptr<http::request> request) -> thinger::awaitable<void> {
            router->handle_request(request);
            co_return;
        });
        
        // Start handling the connection
        connection->start();
    });
    
    // Start the server
    if (!http_server->start()) {
        LOG_ERROR("Failed to start HTTP server on port %s", port.c_str());
        return 1;
    }
    
    LOG_INFO("HTTP Server with Routing listening on http://0.0.0.0:%s", port.c_str());
    LOG_INFO("Try these endpoints:");
    LOG_INFO("  GET    /");
    LOG_INFO("  GET    /api/v1/users");
    LOG_INFO("  GET    /api/v1/users/john_doe              (validates alphanumeric ID)");
    LOG_INFO("  POST   /api/v1/users                      (with JSON body)");
    LOG_INFO("  GET    /api/v1/users/john_doe/devices");
    LOG_INFO("  GET    /api/v1/users/john_doe/devices/device1");
    LOG_INFO("  PUT    /api/v1/users/john_doe/devices/device1");
    LOG_INFO("  DELETE /api/v1/users/john_doe/devices/device1");
    LOG_INFO("  GET    /api/v1/items/123                  (numeric ID only)");
    LOG_INFO("  GET    /api/v1/files/path/to/file.txt     (captures full path)");
    LOG_INFO("  GET    /api/v1/admin/stats                (requires auth)");
    LOG_INFO("Press Ctrl+C to stop");
    
    // Wait for shutdown
    asio::get_workers().wait();
    
    LOG_INFO("Server stopped");
    
    return 0;
}
#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <thinger/util/base64.hpp>
#include <iostream>
#include <map>

using namespace thinger;

// Simple user database
std::map<std::string, std::string> users = {
    {"admin", "secret123"},
    {"user", "password"},
    {"demo", "demo"}
};

// Helper function to parse Basic Auth header
std::pair<std::string, std::string> parse_basic_auth(const std::string& auth_header) {
    // Basic Auth format: "Basic base64(username:password)"
    if (auth_header.size() > 6 && auth_header.substr(0, 6) == "Basic ") {
        std::string encoded = auth_header.substr(6);
        std::string decoded = util::base64::decode(encoded);
        
        size_t colon_pos = decoded.find(':');
        if (colon_pos != std::string::npos) {
            return {
                decoded.substr(0, colon_pos),
                decoded.substr(colon_pos + 1)
            };
        }
    }
    return {"", ""};
}

// Basic Auth middleware
auto create_basic_auth_middleware(const std::string& realm) {
    return [realm](http::request& req, http::response& res, std::function<void()> next) {
        // Get the requested path
        auto uri = req.get_http_request()->get_uri();
        
        // Only protect /admin paths
        if (!uri.starts_with("/admin")) {
            next(); // Not protected, continue
            return;
        }
        
        // Get Authorization header
        std::string auth_header;
        if (req.get_http_request()->has_header("Authorization")) {
            auth_header = req.get_http_request()->get_header("Authorization");
        }
        auto [username, password] = parse_basic_auth(auth_header);
        
        // Check credentials
        auto user_it = users.find(username);
        if (user_it != users.end() && user_it->second == password) {
            // Valid credentials - save username and continue
            req.set_auth_user(username);
            LOG_INFO("User '%s' authenticated for %s", username.c_str(), uri.c_str());
            next();
        } else {
            // Invalid credentials - send 401 with WWW-Authenticate header
            LOG_WARNING("Authentication failed for %s", uri.c_str());
            
            auto http_response = std::make_shared<http::http_response>();
            http_response->set_status(http::http_response::status::unauthorized);
            http_response->add_header("WWW-Authenticate", "Basic realm=\"" + realm + "\"");
            http_response->set_content_type("text/html");
            http_response->set_content(R"(
                <!DOCTYPE html>
                <html>
                <head><title>401 Unauthorized</title></head>
                <body>
                    <h1>401 Unauthorized</h1>
                    <p>You need to authenticate to access this resource.</p>
                    <p>Try username: admin, password: secret123</p>
                </body>
                </html>
            )");
            res.send_response(http_response);
        }
    };
}

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server Example with Basic Auth");
    
    // Create server instance
    http::server server;
    
    // Enable CORS
    server.enable_cors();
    
    // Add Basic Auth middleware
    server.use(create_basic_auth_middleware("Protected Area"));
    
    // Simple routes using different signatures
    
    // Response-only - no need for request data
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head><title>Thinger HTTP Server</title></head>
            <body>
                <h1>Welcome to Thinger HTTP Server!</h1>
                <h2>Public Endpoints:</h2>
                <ul>
                    <li>GET <a href="/health">/health</a> - Health check</li>
                    <li>GET <a href="/api/time">/api/time</a> - Current time</li>
                    <li>GET /api/users/:id - User details</li>
                    <li>POST /api/echo - Echo JSON body</li>
                </ul>
                
                <h2>Protected Area:</h2>
                <ul>
                    <li><a href="/admin">/admin</a> - Admin Dashboard (requires authentication)</li>
                </ul>
                
                <h3>Test Credentials:</h3>
                <ul>
                    <li>Username: <code>admin</code>, Password: <code>secret123</code></li>
                    <li>Username: <code>user</code>, Password: <code>password</code></li>
                    <li>Username: <code>demo</code>, Password: <code>demo</code></li>
                </ul>
            </body>
            </html>
        )");
    });
    
    // Health check endpoint
    server.get("/health", [](http::response& res) {
        res.json({{"status", "ok"}, {"timestamp", std::time(nullptr)}});
    });
    
    // Time endpoint
    server.get("/api/time", [](http::response& res) {
        auto now = std::time(nullptr);
        res.json({
            {"unix", now},
            {"iso", "2024-01-01T00:00:00Z"} // TODO: format properly
        });
    });
    
    // Route with parameters
    server.get("/api/users/:id", [](http::request& req, http::response& res) {
        const auto& user_id = req["id"];
        res.json({
            {"id", user_id},
            {"name", "User " + user_id},
            {"email", user_id + "@example.com"}
        });
    });
    
    // POST with JSON body
    server.post("/api/echo", [](nlohmann::json& body, http::response& res) {
        res.json({
            {"received", body},
            {"timestamp", std::time(nullptr)}
        });
    });
    
    // POST with request and body
    server.post("/api/users/:id/update", [](http::request& req, nlohmann::json& body, http::response& res) {
        const auto& user_id = req["id"];
        res.json({
            {"user_id", user_id},
            {"updated", body},
            {"timestamp", std::time(nullptr)}
        });
    });
    
    // DELETE example
    server.del("/api/users/:id", [](http::request& req, http::response& res) {
        const auto& user_id = req["id"];
        LOG_INFO("Deleting user: %s", user_id.c_str());
        res.json({{"deleted", user_id}});
    });
    
    // ===== PROTECTED ADMIN ROUTES =====
    // These routes require Basic Auth (username/password)
    
    // Admin dashboard
    server.get("/admin", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head><title>Admin Dashboard</title></head>
            <body>
                <h1>Admin Dashboard</h1>
                <p>Welcome, <strong>)" + username + R"(</strong>!</p>
                <p>This is a protected area.</p>
                <ul>
                    <li><a href="/admin/users">User Management</a></li>
                    <li><a href="/admin/stats">System Statistics</a></li>
                    <li><a href="/admin/config">Configuration</a></li>
                </ul>
                <p><a href="/">Back to Home</a></p>
            </body>
            </html>
        )");
    });
    
    // Admin user list
    server.get("/admin/users", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head><title>User Management</title></head>
            <body>
                <h1>User Management</h1>
                <table border="1">
                    <tr><th>Username</th><th>Status</th></tr>
                    <tr><td>admin</td><td>Active</td></tr>
                    <tr><td>user</td><td>Active</td></tr>
                    <tr><td>demo</td><td>Active</td></tr>
                </table>
                <p><a href="/admin">Back to Admin</a></p>
            </body>
            </html>
        )");
    });
    
    // Admin stats (JSON API)
    server.get("/admin/stats", [](http::response& res) {
        res.json({
            {"total_users", 3},
            {"active_sessions", 1},
            {"requests_today", 42},
            {"server_uptime", "2 hours"},
            {"memory_usage", "128MB"}
        });
    });
    
    // Admin config - only 'admin' user can access
    server.get("/admin/config", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        
        // Only 'admin' user can access config
        if (username != "admin") {
            res.error(http::http_response::status::forbidden, 
                     "Only admin user can access configuration");
            return;
        }
        
        res.json({
            {"server_name", "Thinger HTTP Server"},
            {"version", "1.0.0"},
            {"debug_mode", true},
            {"max_connections", 1000}
        });
    });
    
    // Custom 404 handler
    server.set_not_found_handler([](http::request& req, http::response& res) {
        auto uri = req.get_http_request()->get_uri();
        
        if (uri.starts_with("/api/")) {
            res.json({
                {"error", "Not Found"},
                {"path", uri},
                {"message", "The requested API endpoint does not exist"}
            }, http::http_response::status::not_found);
        } else {
            res.html(R"(
                <!DOCTYPE html>
                <html>
                <head><title>404 Not Found</title></head>
                <body>
                    <h1>404 - Page Not Found</h1>
                    <p>The requested page does not exist.</p>
                    <a href="/">Go Home</a>
                </body>
                </html>
            )");
            res.status(http::http_response::status::not_found);
        }
    });
    
    // Get port from command line or use default
    uint16_t port = 8090;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    std::cout << "Server running on http://0.0.0.0:" << port << std::endl;
    std::cout << "Protected area at: http://0.0.0.0:" << port << "/admin" << std::endl;
    std::cout << "Test with: curl -u admin:secret123 http://localhost:" << port << "/admin/stats" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
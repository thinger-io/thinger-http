#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>
#include <map>

using namespace thinger;

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting HTTP Server with Basic Auth Example");
    
    // Create server instance
    http::server server;
    
    // Enable CORS
    server.enable_cors();
    
    // Method 1: Basic Auth with single user
    server.set_basic_auth("/admin", "Admin Area", "admin", "secret123");
    
    // Method 2: Basic Auth with multiple users
    std::map<std::string, std::string> api_users = {
        {"api_user", "api_key_123"},
        {"developer", "dev_pass"},
        {"service", "service_token"}
    };
    server.set_basic_auth("/api/v2", "API v2", api_users);
    
    // Method 3: Basic Auth with custom verification (e.g., database lookup)
    server.set_basic_auth("/secure", "Secure Zone", 
        [](const std::string& username, const std::string& password) {
            // Here you could check against a database, LDAP, etc.
            // For demo, we just check some hardcoded values
            if (username == "power_user" && password == "complex_pass_123") {
                return true;
            }
            // Could also do more complex checks
            if (username.starts_with("guest_") && password == "guest") {
                return true;
            }
            return false;
        });
    
    // Public routes (no auth required)
    server.get("/", [](http::response& res) {
        res.html(R"(
            <!DOCTYPE html>
            <html>
            <head><title>Basic Auth Examples</title></head>
            <body>
                <h1>Basic Auth Configuration Examples</h1>
                
                <h2>Public Area</h2>
                <p>This page is publicly accessible.</p>
                
                <h2>Protected Areas:</h2>
                <ul>
                    <li><a href="/admin">/admin/*</a> - Single user (admin/secret123)</li>
                    <li><a href="/api/v2/users">/api/v2/*</a> - Multiple users (api_user/api_key_123, developer/dev_pass, service/service_token)</li>
                    <li><a href="/secure">/secure/*</a> - Custom verification (power_user/complex_pass_123 or guest_*/guest)</li>
                </ul>
                
                <h2>Test with curl:</h2>
                <pre>
# Single user auth
curl -u admin:secret123 http://localhost:8090/admin

# API users
curl -u api_user:api_key_123 http://localhost:8090/api/v2/users

# Custom verification
curl -u power_user:complex_pass_123 http://localhost:8090/secure/data
curl -u guest_123:guest http://localhost:8090/secure/info
                </pre>
            </body>
            </html>
        )");
    });
    
    server.get("/health", [](http::response& res) {
        res.json({{"status", "ok"}, {"public", true}});
    });
    
    // Admin routes (protected with single user)
    server.get("/admin", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        res.html(R"(
            <h1>Admin Dashboard</h1>
            <p>Welcome, <strong>)" + username + R"(</strong>!</p>
            <p>This area is protected with single user authentication.</p>
        )");
    });
    
    server.get("/admin/settings", [](http::response& res) {
        res.json({
            {"debug_mode", true},
            {"max_connections", 1000},
            {"timeout", 30}
        });
    });
    
    // API v2 routes (protected with multiple users)
    server.get("/api/v2/users", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        res.json({
            {"authenticated_as", username},
            {"users", {"john", "jane", "bob"}},
            {"total", 3}
        });
    });
    
    server.post("/api/v2/data", [](http::request& req, nlohmann::json& body, http::response& res) {
        auto username = req.get_auth_user();
        res.json({
            {"authenticated_as", username},
            {"received", body},
            {"stored", true}
        });
    });
    
    // Secure routes (protected with custom verification)
    server.get("/secure", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        res.html(R"(
            <h1>Secure Zone</h1>
            <p>Welcome, <strong>)" + username + R"(</strong>!</p>
            <p>You passed custom authentication.</p>
        )");
    });
    
    server.get("/secure/data", [](http::request& req, http::response& res) {
        auto username = req.get_auth_user();
        bool is_power_user = (username == "power_user");
        bool is_guest = username.starts_with("guest_");
        
        res.json({
            {"user", username},
            {"access_level", is_power_user ? "full" : (is_guest ? "limited" : "standard")},
            {"permissions", is_power_user ? 
                nlohmann::json::array({"read", "write", "delete", "admin"}) : 
                nlohmann::json::array({"read"})
            }
        });
    });
    
    // Mixed route - public info, protected details
    server.get("/mixed/:resource", [](http::request& req, http::response& res) {
        const auto& resource = req["resource"];
        
        // Check if user is authenticated (will be empty if not)
        auto username = req.get_auth_user();
        
        if (username.empty()) {
            res.json({
                {"resource", resource},
                {"public_info", "This is public information"},
                {"authenticated", false}
            });
        } else {
            res.json({
                {"resource", resource},
                {"public_info", "This is public information"},
                {"authenticated", true},
                {"user", username},
                {"private_data", "Secret information only for authenticated users"}
            });
        }
    });
    
    // Get port from command line or use default
    uint16_t port = 8090;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    std::cout << "Server running on http://0.0.0.0:" << port << std::endl;
    std::cout << "Visit http://localhost:" << port << " to see the example" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
#include <thinger/http_server.hpp>
#include <thinger/util/logger.hpp>
#include <iostream>

using namespace thinger;

// This example demonstrates advanced routing with path parameters
// using the built-in routing system

int main(int argc, char* argv[]) {
    // Initialize logging
    LOG_INFO("Starting Advanced HTTP Server Example");
    
    // Create server instance
    http::server server;
    
    // Root endpoint
    server.get("/", [](http::response& res) {
        res.json({
            {"message", "Welcome to Advanced HTTP Server!"},
            {"endpoints", {
                "/api/v1/users",
                "/api/v1/users/:user_id",
                "/api/v1/users/:user_id/products",
                "/api/v1/users/:user_id/products/:product_id",
                "/api/v1/users/:user_id/products/:product_id/profile/:profile_id"
            }}
        });
    });
    
    // List all users
    server.get("/api/v1/users", [](http::response& res) {
        nlohmann::json users = nlohmann::json::array();
        for (int i = 1; i <= 5; ++i) {
            users.push_back({
                {"id", i},
                {"name", "User " + std::to_string(i)},
                {"email", "user" + std::to_string(i) + "@example.com"}
            });
        }
        res.json({{"users", users}});
    });
    
    // Get specific user
    server.get("/api/v1/users/:user_id", [](http::request& req, http::response& res) {
        auto user_id = req.get_uri_parameter("user_id");
        res.json({
            {"id", user_id},
            {"name", "User " + user_id},
            {"email", "user" + user_id + "@example.com"},
            {"created_at", "2024-01-01"}
        });
    });
    
    // Get user's products
    server.get("/api/v1/users/:user_id/products", [](http::request& req, http::response& res) {
        auto user_id = req.get_uri_parameter("user_id");
        nlohmann::json products = nlohmann::json::array();
        for (int i = 1; i <= 3; ++i) {
            products.push_back({
                {"id", i},
                {"name", "Product " + std::to_string(i)},
                {"owner", user_id}
            });
        }
        res.json({{"products", products}});
    });
    
    // Get specific product
    server.get("/api/v1/users/:user_id/products/:product_id", 
        [](http::request& req, http::response& res) {
            auto user_id = req.get_uri_parameter("user_id");
            auto product_id = req.get_uri_parameter("product_id");
            
            res.json({
                {"id", product_id},
                {"name", "Product " + product_id},
                {"owner", user_id},
                {"description", "This is product " + product_id + " owned by user " + user_id}
            });
        }
    );
    
    // Get product profile - demonstrates multiple path parameters
    server.get("/api/v1/users/:user_id/products/:product_id/profile/:profile_id", 
        [](http::request& req, http::response& res) {
            auto user_id = req.get_uri_parameter("user_id");
            auto product_id = req.get_uri_parameter("product_id");
            auto profile_id = req.get_uri_parameter("profile_id");
            
            res.json({
                {"user_id", user_id},
                {"product_id", product_id},
                {"profile", {
                    {"id", profile_id},
                    {"name", "Profile " + profile_id},
                    {"settings", {
                        {"theme", "dark"},
                        {"notifications", true}
                    }}
                }}
            });
        }
    );
    
    // Create user - demonstrates POST with JSON body
    server.post("/api/v1/users", [](http::request& req, http::response& res) {
        nlohmann::json body;
        try {
            body = nlohmann::json::parse(req.get_http_request()->get_body());
        } catch (const std::exception&) {
            body = nlohmann::json::object();
        }
        
        // In a real app, you'd validate and save the user here
        std::string name = body.value("name", "New User");
        std::string email = body.value("email", "newuser@example.com");
        
        res.status(http::http_response::status::created);
        res.json({
                {"id", 123},
                {"name", name},
                {"email", email},
                {"created_at", std::time(nullptr)}
            });
    });
    
    // Get port from command line or use default
    uint16_t port = 8080;
    if (argc > 1) {
        port = std::stoi(argv[1]);
    }
    
    // Start server
    std::cout << "Advanced HTTP Server listening on http://0.0.0.0:" << port << std::endl;
    std::cout << "Try these endpoints:" << std::endl;
    std::cout << "  GET  /" << std::endl;
    std::cout << "  GET  /api/v1/users" << std::endl;
    std::cout << "  GET  /api/v1/users/123" << std::endl;
    std::cout << "  GET  /api/v1/users/123/products" << std::endl;
    std::cout << "  GET  /api/v1/users/123/products/456" << std::endl;
    std::cout << "  GET  /api/v1/users/123/products/456/profile/789" << std::endl;
    std::cout << "  POST /api/v1/users (with JSON body)" << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    
    // Start server and wait for shutdown
    if (!server.start("0.0.0.0", port)) {
        std::cerr << "Failed to start server on port " << port << std::endl;
        return 1;
    }
    
    return 0;
}
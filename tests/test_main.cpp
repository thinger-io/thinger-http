#define CATCH_CONFIG_MAIN
#include <catch2/catch_all.hpp>
#include <thinger/util/logger.hpp>
#include <cstdlib>

#ifdef THINGER_LOG_SPDLOG

// Global test event listener to initialize logging
class LoggingInitializer : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    
    void testRunStarting(Catch::TestRunInfo const&) override {
        // Initialize logging for all tests using the library's logging system
        static bool initialized = false;
        if (!initialized) {
            // Enable the library's logging
            thinger::logging::enable();
            
            // Set log level based on environment variable or default to warn
            const char* log_level_env = std::getenv("THINGER_LOG_LEVEL");
            if (log_level_env) {
                std::string level_str(log_level_env);
                if (level_str == "trace") {
                    thinger::logging::set_log_level(spdlog::level::trace);
                } else if (level_str == "debug") {
                    thinger::logging::set_log_level(spdlog::level::debug);
                } else if (level_str == "info") {
                    thinger::logging::set_log_level(spdlog::level::info);
                } else if (level_str == "warn") {
                    thinger::logging::set_log_level(spdlog::level::warn);
                } else if (level_str == "error") {
                    thinger::logging::set_log_level(spdlog::level::err);
                } else if (level_str == "critical") {
                    thinger::logging::set_log_level(spdlog::level::critical);
                } else if (level_str == "off") {
                    thinger::logging::set_log_level(spdlog::level::off);
                }
            } else {
                // Default to warn level for tests
                thinger::logging::set_log_level(spdlog::level::warn);
            }
            
            initialized = true;
            
            // Log initialization
            LOG_INFO("Test logging initialized. Level: {}", 
                     log_level_env ? log_level_env : "warn");
        }
    }
};

// Register the listener
CATCH_REGISTER_LISTENER(LoggingInitializer)

#endif // THINGER_LOG_SPDLOG

// Custom main
int main(int argc, char* argv[]) {
    return Catch::Session().run(argc, argv);
}
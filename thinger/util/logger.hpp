#ifndef THINGER_LOGGER_HPP
#define THINGER_LOGGER_HPP
#pragma once

#if __has_include(<spdlog/spdlog.h>)
    #include <spdlog/spdlog.h>
    #include <spdlog/sinks/stdout_color_sinks.h>
    #include <string>
    #include <memory>
    #include <stdexcept>

    #ifndef THINGER_LOG_SPDLOG
        #define THINGER_LOG_SPDLOG
    #endif

    namespace thinger {
        namespace logging {
            // Get/set the logger instance used by the library
            inline std::shared_ptr<spdlog::logger>& get_logger() {
                static std::shared_ptr<spdlog::logger> logger;
                return logger;
            }

            // Set a custom logger for the library
            inline void set_logger(std::shared_ptr<spdlog::logger> logger) {
                get_logger() = logger;
            }

            // Enable logging with default console logger
            inline void enable() {
                auto& logger = get_logger();
                if (!logger) {
                    // Create default console logger with color
                    logger = spdlog::stdout_color_mt("thinger_http");
                    logger->set_level(spdlog::level::info);
                    // Pattern: time [level:8] [thread_id] message
                    // Note: file:line info is often lost when compiled as library
                    logger->set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%^%-8l%$] [%t] %v");
                }
            }

            // Set log level for the library logger
            inline void set_log_level(spdlog::level::level_enum level) {
                if (auto logger = get_logger()) {
                    logger->set_level(level);
                }
            }

            // Disable logging completely
            inline void disable() {
                set_logger(nullptr);
            }
        }
    }

    // Internal macros that use the custom logger if available
    #define THINGER_LOG_IMPL(level, ...) \
        do { \
            if (auto _logger = thinger::logging::get_logger()) { \
                _logger->log(level, __VA_ARGS__); \
            } \
        } while(0)

    // Public logging macros - backward compatible
    #define LOG_INFO(...)     THINGER_LOG_IMPL(spdlog::level::info, __VA_ARGS__)
    #define LOG_ERROR(...)    THINGER_LOG_IMPL(spdlog::level::err, __VA_ARGS__)
    #define LOG_WARNING(...)  THINGER_LOG_IMPL(spdlog::level::warn, __VA_ARGS__)
    #define LOG_DEBUG(...)    THINGER_LOG_IMPL(spdlog::level::debug, __VA_ARGS__)
    #define LOG_TRACE(...)    THINGER_LOG_IMPL(spdlog::level::trace, __VA_ARGS__)
    #define LOG_LEVEL(LEVEL, ...) THINGER_LOG_IMPL(static_cast<spdlog::level::level_enum>(LEVEL), __VA_ARGS__)

    // Thinger specific macros
    #define THINGER_LOG(...)                LOG_INFO(__VA_ARGS__)
    #define THINGER_LOG_TAG(TAG, ...)       LOG_INFO("[{}] " __VA_ARGS__, TAG)
    #define THINGER_LOG_ERROR(...)          LOG_ERROR(__VA_ARGS__)
    #define THINGER_LOG_ERROR_TAG(TAG, ...) LOG_ERROR("[{}] " __VA_ARGS__, TAG)


#elif defined(THINGER_SERIAL_DEBUG)
    #define THINGER_LOG(...) Serial.printf(__VA_ARGS__)
    #define THINGER_LOG_ERROR(...) Serial.printf(__VA_ARGS__)

#else
    #define LOG_INFO(...) void()
    #define LOG_ERROR(...) void()
    #define LOG_WARNING(...) void()
    #define LOG_DEBUG(...) void()
    #define LOG_TRACE(...) void()
    #define LOG_LEVEL(...) void()

    #define THINGER_LOG(...) void()
    #define THINGER_LOG_ERROR(...) void()
    #define THINGER_LOG_TAG(...) void()
    #define THINGER_LOG_ERROR_TAG(...) void()
#endif

#endif
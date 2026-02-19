cmake_minimum_required(VERSION 3.14)
project(thinger_http 
    VERSION 1.0.0
    DESCRIPTION "Thinger HTTP Library - ASIO, HTTP and utilities"
    LANGUAGES CXX C
)

# Set C++ standard
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(THINGER_HTTP_BUILD_SHARED "Build shared library" OFF)
option(THINGER_HTTP_BUILD_TESTS "Build tests" ON)
option(THINGER_HTTP_BUILD_EXAMPLES "Build examples" OFF)
option(THINGER_HTTP_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(THINGER_HTTP_ENABLE_SSL "Enable SSL support" ON)
option(THINGER_HTTP_ENABLE_LOGGING "Enable spdlog logging" ON)
option(THINGER_HTTP_ENABLE_COVERAGE "Enable code coverage" OFF)
option(THINGER_HTTP_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(THINGER_HTTP_ENABLE_FUZZING "Enable fuzz testing with libFuzzer (requires Clang)" OFF)
option(THINGER_HTTP_NO_EXCEPTIONS "Compile without exception support" OFF)

# No-exceptions configuration
if(THINGER_HTTP_NO_EXCEPTIONS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Disabling C++ exceptions")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fno-exceptions")
    else()
        message(WARNING "No-exceptions mode requires GCC or Clang")
    endif()
endif()

# Coverage configuration
if(THINGER_HTTP_ENABLE_COVERAGE)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Enabling code coverage")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} --coverage -fprofile-arcs -ftest-coverage -fprofile-update=atomic")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} --coverage -fprofile-arcs -ftest-coverage -fprofile-update=atomic")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} --coverage")
    else()
        message(WARNING "Code coverage requires GCC or Clang")
    endif()
endif()

# Sanitizers configuration
if(THINGER_HTTP_ENABLE_SANITIZERS)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        message(STATUS "Enabling AddressSanitizer and UndefinedBehaviorSanitizer")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=address,undefined -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    else()
        message(WARNING "Sanitizers require GCC or Clang")
    endif()
endif()

# Fuzzing configuration (requires Clang)
if(THINGER_HTTP_ENABLE_FUZZING)
    if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(STATUS "Enabling fuzz testing with libFuzzer")
        set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer")
        set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer")
        set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
    else()
        message(FATAL_ERROR "Fuzz testing requires Clang (libFuzzer)")
    endif()
endif()

# Find dependencies
find_package(Threads REQUIRED)
find_package(Boost REQUIRED COMPONENTS iostreams)
find_package(ZLIB REQUIRED)  # Required by Boost::iostreams for compression

if(THINGER_HTTP_ENABLE_SSL)
    find_package(OpenSSL REQUIRED)
endif()

# Fetch nlohmann_json
include(FetchContent)
FetchContent_Declare(
    json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG v3.11.3
)
FetchContent_MakeAvailable(json)

if(THINGER_HTTP_ENABLE_LOGGING)
    find_package(spdlog QUIET)
    if(NOT spdlog_FOUND)
        message(STATUS "spdlog not found, fetching from GitHub...")
        FetchContent_Declare(
            spdlog
            GIT_REPOSITORY https://github.com/gabime/spdlog.git
            GIT_TAG        v1.12.0
        )
        FetchContent_MakeAvailable(spdlog)
    endif()
endif()

# Collect source files
file(GLOB_RECURSE THINGER_SOURCES 
    ${CMAKE_CURRENT_SOURCE_DIR}/thinger/*.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thinger/*.c
)

file(GLOB_RECURSE THINGER_HEADERS 
    ${CMAKE_CURRENT_SOURCE_DIR}/thinger/*.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/thinger/*.h
)

# Create library
if(THINGER_HTTP_BUILD_SHARED)
    add_library(thinger_http SHARED ${THINGER_SOURCES})
else()
    add_library(thinger_http STATIC ${THINGER_SOURCES})
endif()

# Alias for cleaner usage
add_library(thinger::http ALIAS thinger_http)

# Set include directories
target_include_directories(thinger_http
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:include>
)

# Link dependencies
target_link_libraries(thinger_http
    PUBLIC
        Threads::Threads
        Boost::iostreams
        ZLIB::ZLIB
        nlohmann_json::nlohmann_json
        $<$<BOOL:${THINGER_HTTP_ENABLE_SSL}>:OpenSSL::SSL>
        $<$<BOOL:${THINGER_HTTP_ENABLE_SSL}>:OpenSSL::Crypto>
    PRIVATE
        $<$<BOOL:${THINGER_HTTP_ENABLE_LOGGING}>:spdlog::spdlog>
)

# Compile definitions
if(THINGER_HTTP_ENABLE_SSL)
    target_compile_definitions(thinger_http PUBLIC THINGER_HTTP_SSL_ENABLED)
endif()

if(THINGER_HTTP_ENABLE_LOGGING)
    target_compile_definitions(thinger_http PUBLIC THINGER_LOG_SPDLOG)
endif()

if(THINGER_HTTP_NO_EXCEPTIONS)
    target_compile_definitions(thinger_http PUBLIC THINGER_HTTP_NO_EXCEPTIONS)
endif()

# Disable logging when building benchmarks
if(THINGER_HTTP_BUILD_BENCHMARKS)
    target_compile_definitions(thinger_http PUBLIC THINGER_NO_AUTO_LOGGER_INIT)
endif()

# Set properties
set_target_properties(thinger_http PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    PUBLIC_HEADER "${THINGER_HEADERS}"
)

# Testing
if(THINGER_HTTP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
    
    # Coverage target
    if(THINGER_HTTP_ENABLE_COVERAGE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        find_program(LCOV lcov)
        find_program(GENHTML genhtml)
        
        if(LCOV AND GENHTML)
            add_custom_target(coverage
                COMMAND ${CMAKE_COMMAND} -E remove_directory ${CMAKE_BINARY_DIR}/coverage_report
                COMMAND ${CMAKE_COMMAND} -E make_directory ${CMAKE_BINARY_DIR}/coverage_report
                COMMAND ${LCOV} --zerocounters --directory .
                COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure
                COMMAND ${LCOV} --capture --directory . --output-file coverage.info
                COMMAND ${LCOV} --remove coverage.info '/usr/*' '*/build/*' '*/tests/*' '*/_deps/*' --output-file coverage_filtered.info
                COMMAND ${GENHTML} coverage_filtered.info --output-directory coverage_report
                COMMAND ${CMAKE_COMMAND} -E echo "Coverage report generated in ${CMAKE_BINARY_DIR}/coverage_report/index.html"
                WORKING_DIRECTORY ${CMAKE_BINARY_DIR}
                DEPENDS ${THINGER_HTTP_TEST_TARGETS}
                COMMENT "Running tests and generating coverage report"
            )
        else()
            message(STATUS "lcov/genhtml not found - coverage target will not be available")
        endif()
    endif()
endif()

# Examples
if(THINGER_HTTP_BUILD_EXAMPLES)
    add_subdirectory(examples)
endif()

# Benchmarks
if(THINGER_HTTP_BUILD_BENCHMARKS)
    add_subdirectory(benchmark)
endif()

# Fuzzing
if(THINGER_HTTP_ENABLE_FUZZING)
    add_subdirectory(tests/fuzz)
endif()

# Installation (commented out for now)
# include(GNUInstallDirs)
# include(CMakePackageConfigHelpers)
# 
# # Install library
# install(TARGETS thinger_http
#     EXPORT thinger_http-targets
#     LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
#     RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
# )
# 
# # Install headers maintaining directory structure
# install(DIRECTORY thinger/
#     DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/thinger
#     FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
# )
# 
# # Export targets
# install(EXPORT thinger_http-targets
#     FILE ThingerHttpTargets.cmake
#     NAMESPACE thinger::
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thinger_http
# )
# 
# # Create config file
# configure_package_config_file(
#     ${CMAKE_CURRENT_SOURCE_DIR}/cmake/ThingerHttpConfig.cmake.in
#     ${CMAKE_CURRENT_BINARY_DIR}/ThingerHttpConfig.cmake
#     INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thinger_http
# )
# 
# # Create version file
# write_basic_package_version_file(
#     ${CMAKE_CURRENT_BINARY_DIR}/ThingerHttpConfigVersion.cmake
#     VERSION ${PROJECT_VERSION}
#     COMPATIBILITY SameMajorVersion
# )
# 
# # Install config files
# install(FILES
#     ${CMAKE_CURRENT_BINARY_DIR}/ThingerHttpConfig.cmake
#     ${CMAKE_CURRENT_BINARY_DIR}/ThingerHttpConfigVersion.cmake
#     DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/thinger_http
# )

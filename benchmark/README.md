# HTTP Framework Benchmarks

This directory contains performance benchmarks comparing thinger-http with other popular C++ HTTP frameworks.

## Frameworks Tested

Each framework runs an identical "Hello World!" endpoint:

**thinger-http** (port 9080)
```cpp
http::pool_server srv;
srv.get("/", [](http::request& req, http::response& res) {
    res.send("Hello World!");
});
srv.start("0.0.0.0", 9080);
```

**cpp-httplib** (port 9081)
```cpp
httplib::Server svr;
svr.Get("/", [](const httplib::Request&, httplib::Response& res) {
    res.set_content("Hello World!", "text/plain");
});
svr.listen("0.0.0.0", 9081);
```

**Crow** (port 9082)
```cpp
crow::SimpleApp app;
CROW_ROUTE(app, "/")([]() { return "Hello World!"; });
app.port(9082).multithreaded().run();
```

## Prerequisites

### Required Tools
- CMake 3.14 or higher
- C++20 compatible compiler
- bombardier (HTTP benchmarking tool)

### Install bombardier
```bash
# macOS
brew install bombardier

# Linux
wget https://github.com/codesenberg/bombardier/releases/download/v1.2.6/bombardier-linux-amd64
chmod +x bombardier-linux-amd64
sudo mv bombardier-linux-amd64 /usr/local/bin/bombardier
```

## Building

All frameworks are automatically downloaded and built using CMake with identical optimization settings (-O3 Release mode).

### Option 1: Using Make (Recommended)
From the benchmark directory:
```bash
cd thinger-http/benchmark
make build
```

This will automatically:
1. Configure the main project with benchmarks enabled
2. Download dependencies (Asio, cpp-httplib, Crow)
3. Build all benchmark executables

### Option 2: Using CMake directly
From the project root directory:
```bash
cd thinger-http
cmake -B build -DTHINGER_HTTP_BUILD_BENCHMARKS=ON -DCMAKE_BUILD_TYPE=Release
cmake --build build --target benchmark_thinger_http benchmark_cpp_httplib benchmark_crow
```

Note: The Makefile in the benchmark directory expects to be run from `thinger-http/benchmark/` and will automatically handle the CMake configuration from the parent directory.

## Running Benchmarks

All benchmark commands should be run from the benchmark directory:
```bash
cd thinger-http/benchmark
```

### Individual Framework Benchmarks
```bash
# Benchmark thinger-http
make bench-thinger

# Benchmark cpp-httplib
make bench-httplib

# Benchmark Crow
make bench-crow
```

### Run All Benchmarks
```bash
make bench-all
```

This will run bombardier against each framework sequentially with:
- 100 concurrent connections
- 10 second duration
- Simple "Hello World!" endpoint

### Manual Testing
To run all servers simultaneously for manual testing:
```bash
make run-all
```

Then you can access:
- http://localhost:9080 (thinger-http)
- http://localhost:9081 (cpp-httplib)
- http://localhost:9082 (Crow)

Press Ctrl+C to stop all servers.

## Understanding Results

bombardier output shows:
- **Reqs/sec**: Requests per second (higher is better)
- **Latency**: Response time statistics
  - Avg: Average response time
  - Stdev: Standard deviation
  - Max: Maximum response time
  - +/- Stdev: Percentage within 1 standard deviation
- **HTTP codes**: Should show all 200 OK for successful runs
- **Throughput**: Data transfer rate

## Clean Up

From the benchmark directory:
```bash
make clean
```

Note: This only cleans the local benchmark build directory. To fully clean the project build, run from the project root:
```bash
rm -rf build
```

## Sample Results

On Apple M2 Max (macOS), 100 concurrent connections, 10s duration, `-O3` Release mode:

| Framework | Req/s | Avg Latency | Throughput |
|---|--:|--:|--:|
| **thinger-http** | **~131,000** | **0.76ms** | 20.4 MB/s |
| Crow | ~122,000 | 0.82ms | 22.7 MB/s |
| cpp-httplib | ~34,000 | 3.89ms | 3.6 MB/s |

Both thinger-http and Crow run multi-threaded. thinger-http is approximately 3.9x faster than cpp-httplib.

## Notes

- All frameworks are compiled with the same C++20 standard and -O3 optimization
- Each framework runs on a different port to allow simultaneous testing
- The benchmark tests a simple "Hello World!" endpoint for fair comparison
- Results may vary based on system load and hardware capabilities
- Logging is disabled for benchmarks to ensure fair performance comparison
#!/bin/bash

# Run benchmarks script for thinger-http
# This script provides an easy way to run various benchmark configurations

set -e  # Exit on error

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Default values
TOOL="bombardier"
CONNECTIONS=100
DURATION="10s"
THREADS=12

# Function to print colored output
print_color() {
    local color=$1
    local message=$2
    echo -e "${color}${message}${NC}"
}

# Function to show usage
usage() {
    cat << EOF
Usage: $0 [OPTIONS] [COMMAND]

Commands:
  all         Run all benchmarks (default)
  thinger     Run thinger-http benchmark only
  httplib     Run cpp-httplib benchmark only
  crow        Run Crow benchmark only
  compare     Run all benchmarks and show comparison
  clean       Clean build artifacts
  build       Build benchmarks only

Options:
  -t, --tool TOOL        Benchmark tool to use: bombardier or wrk (default: bombardier)
  -c, --connections NUM  Number of concurrent connections (default: 200)
  -d, --duration TIME    Duration of benchmark (default: 10s)
  --threads NUM          Number of threads for wrk (default: 12)
  -h, --help            Show this help message

Examples:
  $0                     # Run all benchmarks with bombardier
  $0 -t wrk              # Run all benchmarks with wrk
  $0 -c 100 -d 30s       # Run with 100 connections for 30 seconds
  $0 thinger             # Run only thinger-http benchmark
  $0 compare             # Run all and show comparison table

EOF
}

# Parse command line arguments
COMMAND=""
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--tool)
            TOOL="$2"
            shift 2
            ;;
        -c|--connections)
            CONNECTIONS="$2"
            shift 2
            ;;
        -d|--duration)
            DURATION="$2"
            shift 2
            ;;
        --threads)
            THREADS="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        all|thinger|httplib|crow|compare|clean|build)
            COMMAND="$1"
            shift
            ;;
        *)
            print_color $RED "Unknown option: $1"
            usage
            exit 1
            ;;
    esac
done

# Default command is 'all'
if [ -z "$COMMAND" ]; then
    COMMAND="all"
fi

# Validate tool
if [ "$TOOL" != "bombardier" ] && [ "$TOOL" != "wrk" ]; then
    print_color $RED "Invalid tool: $TOOL. Must be 'bombardier' or 'wrk'"
    exit 1
fi

# Check if tool is installed
if ! command -v $TOOL &> /dev/null; then
    print_color $RED "$TOOL is not installed!"
    if [ "$TOOL" = "bombardier" ]; then
        echo "Install with: brew install bombardier (macOS) or download from GitHub"
    else
        echo "Install with: brew install wrk (macOS) or apt-get install wrk (Linux)"
    fi
    exit 1
fi

# Function to build benchmarks
build_benchmarks() {
    print_color $BLUE "Building benchmarks..."
    make build
}

# Function to run a single benchmark
run_benchmark() {
    local name=$1
    local port=$2
    local binary=$3
    
    print_color $YELLOW "\n=== Benchmarking $name (port $port) ==="
    
    # Start the server
    ../build/benchmark/$binary &
    local pid=$!
    
    # Wait for server to start
    sleep 2
    
    # Run the benchmark
    if [ "$TOOL" = "bombardier" ]; then
        bombardier -c $CONNECTIONS -d $DURATION http://localhost:$port/
    else
        wrk -t$THREADS -c$CONNECTIONS -d$DURATION --latency http://localhost:$port/
    fi
    
    # Kill the server
    kill $pid 2>/dev/null || true
    wait $pid 2>/dev/null || true
}

# Function to run all benchmarks and collect results
run_compare() {
    print_color $GREEN "\n📊 Running comparative benchmarks..."
    print_color $BLUE "Configuration: $CONNECTIONS connections, $DURATION duration, tool: $TOOL\n"
    
    # Create temporary files for results
    local thinger_result=$(mktemp)
    local httplib_result=$(mktemp)
    local crow_result=$(mktemp)
    
    # Run benchmarks and capture output
    print_color $YELLOW "Running thinger-http..."
    run_benchmark "thinger-http" 9080 "benchmark_thinger_http" > $thinger_result 2>&1
    
    print_color $YELLOW "Running cpp-httplib..."
    run_benchmark "cpp-httplib" 9081 "benchmark_cpp_httplib" > $httplib_result 2>&1
    
    print_color $YELLOW "Running Crow..."
    run_benchmark "Crow" 9082 "benchmark_crow" > $crow_result 2>&1
    
    # Extract and display results
    print_color $GREEN "\n📈 Benchmark Results Summary:"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    printf "%-15s | %-15s | %-15s\n" "Framework" "Requests/sec" "Avg Latency"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    # Extract metrics based on tool
    if [ "$TOOL" = "bombardier" ]; then
        # Extract from bombardier output
        local thinger_rps=$(grep "Reqs/sec" $thinger_result | awk '{print $2}')
        local thinger_lat=$(grep "Latency" $thinger_result -A1 | tail -1 | awk '{print $2}')
        
        local httplib_rps=$(grep "Reqs/sec" $httplib_result | awk '{print $2}')
        local httplib_lat=$(grep "Latency" $httplib_result -A1 | tail -1 | awk '{print $2}')
        
        local crow_rps=$(grep "Reqs/sec" $crow_result | awk '{print $2}')
        local crow_lat=$(grep "Latency" $crow_result -A1 | tail -1 | awk '{print $2}')
    else
        # Extract from wrk output
        local thinger_rps=$(grep "Requests/sec:" $thinger_result | awk '{print $2}')
        local thinger_lat=$(grep "Latency" $thinger_result | head -1 | awk '{print $2}')
        
        local httplib_rps=$(grep "Requests/sec:" $httplib_result | awk '{print $2}')
        local httplib_lat=$(grep "Latency" $httplib_result | head -1 | awk '{print $2}')
        
        local crow_rps=$(grep "Requests/sec:" $crow_result | awk '{print $2}')
        local crow_lat=$(grep "Latency" $crow_result | head -1 | awk '{print $2}')
    fi
    
    printf "%-15s | %-15s | %-15s\n" "thinger-http" "$thinger_rps" "$thinger_lat"
    printf "%-15s | %-15s | %-15s\n" "cpp-httplib" "$httplib_rps" "$httplib_lat"
    printf "%-15s | %-15s | %-15s\n" "Crow" "$crow_rps" "$crow_lat"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    
    # Clean up temp files
    rm -f $thinger_result $httplib_result $crow_result
}

# Main execution
case $COMMAND in
    build)
        build_benchmarks
        ;;
    clean)
        print_color $BLUE "Cleaning build artifacts..."
        make clean
        ;;
    all)
        build_benchmarks
        run_benchmark "thinger-http" 9080 "benchmark_thinger_http"
        run_benchmark "cpp-httplib" 9081 "benchmark_cpp_httplib"
        run_benchmark "Crow" 9082 "benchmark_crow"
        print_color $GREEN "\n✅ All benchmarks completed!"
        ;;
    thinger)
        build_benchmarks
        run_benchmark "thinger-http" 9080 "benchmark_thinger_http"
        ;;
    httplib)
        build_benchmarks
        run_benchmark "cpp-httplib" 9081 "benchmark_cpp_httplib"
        ;;
    crow)
        build_benchmarks
        run_benchmark "Crow" 9082 "benchmark_crow"
        ;;
    compare)
        build_benchmarks
        run_compare
        ;;
    *)
        print_color $RED "Unknown command: $COMMAND"
        usage
        exit 1
        ;;
esac
#!/bin/bash
#
# run_coverage.sh - Run tests and generate coverage report in Docker
#

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DOCKER_IMAGE="thinger/compiler:mold-6.0.0"
BUILD_DIR="build-coverage"

# Colors
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

echo -e "${YELLOW}Running coverage in Docker: ${DOCKER_IMAGE}${NC}"

docker run --rm \
    -v "${SCRIPT_DIR}:/src" \
    -w /src \
    ${DOCKER_IMAGE} \
    bash -c "
        set -e

        echo '=== Thinger HTTP Coverage Report ==='
        echo ''

        # Clean build directory to avoid cache conflicts between local/docker
        rm -rf ${BUILD_DIR}
        mkdir -p ${BUILD_DIR}
        cd ${BUILD_DIR}

        echo '[1/5] Configuring CMake with coverage...'
        cmake -DTHINGER_HTTP_ENABLE_COVERAGE=ON \
              -DTHINGER_HTTP_BUILD_TESTS=ON \
              -DTHINGER_HTTP_BUILD_EXAMPLES=OFF \
              -DTHINGER_HTTP_BUILD_BENCHMARKS=OFF \
              -DCMAKE_BUILD_TYPE=Debug \
              ..

        echo ''
        echo '[2/5] Building...'
        make -j\$(nproc)

        echo ''
        echo '[3/5] Cleaning previous coverage data...'
        find . -name '*.gcda' -delete 2>/dev/null || true
        lcov --zerocounters --directory . 2>/dev/null || true

        echo ''
        echo '[4/5] Running tests...'
        ./tests/all_tests || true

        echo ''
        echo '[5/5] Generating coverage report...'

        # Capture coverage data (only from thinger library, not tests or deps)
        lcov --capture \
            --directory ./CMakeFiles/thinger_http.dir \
            --output-file coverage.info \
            --ignore-errors mismatch,gcov,negative

        # Remove unwanted paths from coverage
        lcov --remove coverage.info \
            '*/tests/*' \
            '*/_deps/*' \
            '/usr/*' \
            --output-file coverage_final.info \
            --ignore-errors unused

        # Generate HTML report
        genhtml coverage_final.info \
            --output-directory coverage_report \
            --title 'Thinger HTTP Library Coverage' \
            --legend \
            --show-details

        echo ''
        echo '=== Coverage Summary ==='
        lcov --summary coverage_final.info

        echo ''
        echo 'Report generated: ${BUILD_DIR}/coverage_report/index.html'
    "

echo ""
echo -e "${GREEN}Coverage report: ${SCRIPT_DIR}/${BUILD_DIR}/coverage_report/index.html${NC}"

# Open in browser on macOS
if [[ "$OSTYPE" == "darwin"* ]]; then
    open "${SCRIPT_DIR}/${BUILD_DIR}/coverage_report/index.html"
fi

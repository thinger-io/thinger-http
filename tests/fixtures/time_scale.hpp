#ifndef THINGER_HTTP_TEST_TIME_SCALE_HPP
#define THINGER_HTTP_TEST_TIME_SCALE_HPP

#include <chrono>

// What instrumentation adds to the time bounds of the tests. A build with AddressSanitizer,
// ThreadSanitizer or code coverage runs several times slower and its timers fire later: on a
// loaded machine a 50 ms bound was seen to take 60 ms, and a 100 ms one 157 ms, with nothing wrong
// in the library. So the tolerance of a test (never a bound of the library) is scaled() in those
// builds, and stays as it is in any other.
//
// It is told when compiling: the sanitizers by the macros of the compiler (GCC defines
// __SANITIZE_*, clang answers __has_feature) and coverage, which has no macro, by the definition
// tests/CMakeLists.txt adds with THINGER_HTTP_ENABLE_COVERAGE. UBSan alone has no macro on GCC and
// adds little time: THINGER_HTTP_ENABLE_SANITIZERS always brings AddressSanitizer with it.
//
// A scaled tolerance must stay below what its check has to tell apart: keep a static_assert by it.
// The tightest today is in shutdown_deadline_test.cpp, which bounds this value.

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || defined(THINGER_HTTP_TEST_COVERAGE)
#define THINGER_HTTP_TEST_INSTRUMENTED 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define THINGER_HTTP_TEST_INSTRUMENTED 1
#endif
#endif

namespace thinger::http::test {

#ifdef THINGER_HTTP_TEST_INSTRUMENTED
    inline constexpr int time_scale = 3;
#else
    inline constexpr int time_scale = 1;
#endif

    constexpr std::chrono::milliseconds scaled(std::chrono::milliseconds bound) {
        return bound * time_scale;
    }

}

#undef THINGER_HTTP_TEST_INSTRUMENTED

#endif
